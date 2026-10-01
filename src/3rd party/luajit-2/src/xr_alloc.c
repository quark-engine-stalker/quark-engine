#include "xr_alloc.h"
#include "lj_def.h"
#include "lj_arch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef long (*PNTAVM)(HANDLE handle, void **addr, ULONG_PTR zbits,
                       size_t *size, ULONG alloctype, ULONG prot);
extern PNTAVM ntavm;

/* LuaJIT 2.0 on x64 stores GC references in the lower 2 GB. Reserve the pool
** before engine/DLL initialization fragments that address range.
*/
#define NTAVM_ZEROBITS 1

#define MAX_SIZE_T (~(size_t)0)
#define MFAIL ((void *)(MAX_SIZE_T))

#define CHUNK_SIZE ((size_t)64 * 1024)
#define REQUESTED_POOL_MB 1664
#define MAX_CHUNK_COUNT ((REQUESTED_POOL_MB * 1024) / 64)
#define CHUNKS_FROM_SIZE(x) (((x) + CHUNK_SIZE - 1) / CHUNK_SIZE)

static volatile LONG g_init_state = 0; /* 0=not started, 1=initializing, 2=ready */
static void* g_heap = NULL;
static size_t g_chunk_count = 0;
static unsigned char g_heap_map[MAX_CHUNK_COUNT];
static size_t g_first_free_chunk = 0;
static size_t g_committed_chunks = 0;
static size_t g_peak_committed_chunks = 0;
static size_t g_fallback_bytes = 0;
static size_t g_peak_fallback_bytes = 0;
static size_t g_fallback_active_allocations = 0;
static size_t g_fallback_total_allocations = 0;
static size_t g_allocation_failures = 0;
static SRWLOCK g_heap_lock = SRWLOCK_INIT;

static const unsigned short g_pool_candidates_mb[] =
{
    REQUESTED_POOL_MB, 1536, 1280, 1024, 768, 512
};

static void debug_pool_message(const char* prefix, size_t capacity_mb)
{
#ifdef _DEBUG
    char buffer[160];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
        "LuaJIT pool: %s %Iu MiB at %p\r\n", prefix, capacity_mb, g_heap);
    OutputDebugStringA(buffer);
#else
    (void)prefix;
    (void)capacity_mb;
#endif
}

static size_t find_free_chunks(size_t count)
{
    if (!g_heap || !count || count > g_chunk_count)
        return MAX_SIZE_T;

    size_t run = 0;
    size_t begin = g_first_free_chunk < g_chunk_count ? g_first_free_chunk : 0;

    for (size_t index = begin; index < g_chunk_count; ++index)
    {
        if (g_heap_map[index] == 0)
        {
            if (++run == count)
                return index + 1 - count;
        }
        else
        {
            run = 0;
        }
    }

    /* Fragmentation can move the first suitable run before the hint. */
    run = 0;
    for (size_t index = 0; index < begin; ++index)
    {
        if (g_heap_map[index] == 0)
        {
            if (++run == count)
                return index + 1 - count;
        }
        else
        {
            run = 0;
        }
    }

    return MAX_SIZE_T;
}

static void refresh_first_free_hint(size_t from)
{
    for (size_t index = from; index < g_chunk_count; ++index)
    {
        if (g_heap_map[index] == 0)
        {
            g_first_free_chunk = index;
            return;
        }
    }
    g_first_free_chunk = g_chunk_count;
}

static void record_allocation_failure(void)
{
    AcquireSRWLockExclusive(&g_heap_lock);
    ++g_allocation_failures;
    ReleaseSRWLockExclusive(&g_heap_lock);
}

/* NtAllocateVirtualMemory's ZeroBits argument is not, by itself, a strict
** lower-2-GB bound on every 64-bit Windows version. Prefer a documented scan
** of free regions and pass an explicit address to VirtualAlloc. The native
** call remains a guarded fallback for compatibility with the legacy path.
*/
static void* allocate_explicit_low_region(
    size_t allocation_size, DWORD allocation_type, DWORD protection)
{
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);

    const uintptr_t address_limit = (uintptr_t)1 << 31;
    const uintptr_t granularity = system_info.dwAllocationGranularity ?
        (uintptr_t)system_info.dwAllocationGranularity : (uintptr_t)CHUNK_SIZE;
    uintptr_t cursor = (uintptr_t)system_info.lpMinimumApplicationAddress;

    while (cursor < address_limit)
    {
        MEMORY_BASIC_INFORMATION memory_info;
        if (VirtualQuery((void*)cursor, &memory_info, sizeof(memory_info)) == 0)
            break;

        const uintptr_t region_begin = (uintptr_t)memory_info.BaseAddress;
        const uintptr_t region_end = region_begin + memory_info.RegionSize;
        if (region_end <= cursor || region_end < region_begin)
            break;

        if (memory_info.State == MEM_FREE)
        {
            uintptr_t aligned_begin =
                (region_begin + granularity - 1u) & ~(granularity - 1u);

            /* Leave one allocation-granularity guard after the main pool.
            ** dlmalloc merges exactly adjacent mappings; keeping them separate
            ** preserves the distinct pool-decommit/fallback-release rules.
            */
            if (g_heap)
            {
                const uintptr_t pool_end =
                    (uintptr_t)g_heap + g_chunk_count * CHUNK_SIZE;
                if (aligned_begin == pool_end &&
                    aligned_begin <= address_limit - granularity)
                {
                    aligned_begin += granularity;
                }
            }

            const uintptr_t pool_begin = (uintptr_t)g_heap;
            const int ends_at_pool = g_heap &&
                allocation_size <= address_limit - aligned_begin &&
                aligned_begin + allocation_size == pool_begin;
            if (aligned_begin >= region_begin && aligned_begin < region_end &&
                allocation_size <= region_end - aligned_begin &&
                allocation_size <= address_limit - aligned_begin &&
                !ends_at_pool)
            {
                void* const allocation = VirtualAlloc(
                    (void*)aligned_begin, allocation_size,
                    allocation_type, protection);
                if (allocation)
                    return allocation;
            }
        }

        cursor = region_end;
    }

    return NULL;
}

static void* allocate_native_low_region(
    size_t allocation_size, ULONG allocation_type, ULONG protection,
    size_t* actual_size)
{
    if (!ntavm)
        return NULL;

    void* base = NULL;
    size_t reserve_size = allocation_size;
    const long status = ntavm(
        INVALID_HANDLE_VALUE,
        &base,
        NTAVM_ZEROBITS,
        &reserve_size,
        allocation_type,
        protection);

    if (status < 0 || !base)
    {
        if (base)
            VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }

    const uintptr_t allocation_begin = (uintptr_t)base;
    const uintptr_t allocation_end = allocation_begin + reserve_size;
    if (allocation_end > ((uintptr_t)1 << 31) ||
        allocation_end < allocation_begin)
    {
        VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }

    if (actual_size)
        *actual_size = reserve_size;
    return base;
}

/* Keep LuaJIT usable if a single large reservation cannot be created (ASLR,
** overlays and injected DLLs can fragment the lower address range before
** WinMain). These mappings retain LuaJIT 2.0's strict lower-2-GB guarantee.
*/
static void* allocate_low_address_fallback(size_t allocation_size)
{
    if (!allocation_size || allocation_size > ((size_t)1 << 31))
    {
        record_allocation_failure();
        return MFAIL;
    }

    size_t reserve_size = allocation_size;
    void* base = allocate_explicit_low_region(
        allocation_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base)
        base = allocate_native_low_region(
            allocation_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE,
            &reserve_size);

    if (base)
    {
        const uintptr_t allocation_begin = (uintptr_t)base;
        const uintptr_t allocation_end = allocation_begin + reserve_size;
        const uintptr_t pool_begin = (uintptr_t)g_heap;
        const uintptr_t pool_end = pool_begin + g_chunk_count * CHUNK_SIZE;

        /* Adjacent mappings could be coalesced by dlmalloc into one segment.
        ** Reject adjacency to the reserved pool so a later partial trim never
        ** mixes MEM_DECOMMIT (pool) and MEM_RELEASE (fallback) semantics.
        */
        const int touches_pool = g_heap &&
            (allocation_end == pool_begin || allocation_begin == pool_end);
        if (allocation_end <= ((uintptr_t)1 << 31) &&
            allocation_end >= allocation_begin && !touches_pool)
        {
            AcquireSRWLockExclusive(&g_heap_lock);
            g_fallback_bytes += reserve_size;
            if (g_fallback_bytes > g_peak_fallback_bytes)
                g_peak_fallback_bytes = g_fallback_bytes;
            ++g_fallback_active_allocations;
            ++g_fallback_total_allocations;
            ReleaseSRWLockExclusive(&g_heap_lock);
            return base;
        }
    }

    if (base)
        VirtualFree(base, 0, MEM_RELEASE);
    record_allocation_failure();
    return MFAIL;
}

static int validate_fallback_range(unsigned char* address, size_t size)
{
    while (size)
    {
        MEMORY_BASIC_INFORMATION memory_info;
        if (VirtualQuery(address, &memory_info, sizeof(memory_info)) == 0 ||
            memory_info.BaseAddress != address ||
            memory_info.AllocationBase != address ||
            memory_info.State != MEM_COMMIT ||
            memory_info.RegionSize > size)
        {
            return 0;
        }

        address += memory_info.RegionSize;
        size -= memory_info.RegionSize;
    }

    return 1;
}

static int release_fallback_range(unsigned char* address, size_t size)
{
    if (!validate_fallback_range(address, size))
        return -1;

    while (size)
    {
        MEMORY_BASIC_INFORMATION memory_info;
        if (VirtualQuery(address, &memory_info, sizeof(memory_info)) == 0)
            return -1;

        const size_t region_size = memory_info.RegionSize;
        if (!VirtualFree(address, 0, MEM_RELEASE))
            return -1;

        AcquireSRWLockExclusive(&g_heap_lock);
        g_fallback_bytes = g_fallback_bytes >= region_size ?
            g_fallback_bytes - region_size : 0;
        if (g_fallback_active_allocations)
            --g_fallback_active_allocations;
        ReleaseSRWLockExclusive(&g_heap_lock);

        address += region_size;
        size -= region_size;
    }

    return 0;
}

void XR_INIT(void)
{
    const LONG previous_state = InterlockedCompareExchange(&g_init_state, 1, 0);
    if (previous_state == 2)
        return;
    if (previous_state == 1)
    {
        while (InterlockedCompareExchange(&g_init_state, 0, 0) == 1)
            SwitchToThread();
        if (InterlockedCompareExchange(&g_init_state, 0, 0) == 0)
            XR_INIT();
        return;
    }

    memset(g_heap_map, 0, sizeof(g_heap_map));

    for (size_t candidate = 0;
         candidate < sizeof(g_pool_candidates_mb) / sizeof(g_pool_candidates_mb[0]);
         ++candidate)
    {
        const size_t requested_mb = g_pool_candidates_mb[candidate];
        size_t reserve_size = requested_mb * 1024u * 1024u;
        void* base = allocate_explicit_low_region(
            reserve_size, MEM_RESERVE, PAGE_NOACCESS);
        if (!base)
            base = allocate_native_low_region(
                reserve_size, MEM_RESERVE, PAGE_NOACCESS, &reserve_size);

        if (base)
        {
            const uintptr_t pool_begin = (uintptr_t)base;
            const uintptr_t pool_end = pool_begin + reserve_size;
            if (pool_end > ((uintptr_t)1 << 31) || pool_end < pool_begin)
            {
                VirtualFree(base, 0, MEM_RELEASE);
                continue;
            }

            g_heap = base;
            g_chunk_count = reserve_size / CHUNK_SIZE;
            if (g_chunk_count > MAX_CHUNK_COUNT)
                g_chunk_count = MAX_CHUNK_COUNT;
            g_first_free_chunk = 0;
            debug_pool_message(
                requested_mb == REQUESTED_POOL_MB ? "reserved requested" : "reserved fallback",
                (g_chunk_count * CHUNK_SIZE) / (1024u * 1024u));
            InterlockedExchange(&g_init_state, 2);
            return;
        }

        if (base)
            VirtualFree(base, 0, MEM_RELEASE);
    }

    debug_pool_message("reserve failed", 0);
    InterlockedExchange(&g_init_state, 2);
}

void* XR_MMAP(size_t size)
{
    if (InterlockedCompareExchange(&g_init_state, 0, 0) != 2)
        XR_INIT();

    if (!size || size > MAX_SIZE_T - (CHUNK_SIZE - 1))
        return MFAIL;

    const size_t chunks = CHUNKS_FROM_SIZE(size);
    const size_t allocation_size = chunks * CHUNK_SIZE;

    if (g_heap && chunks && chunks <= g_chunk_count)
    {
        AcquireSRWLockExclusive(&g_heap_lock);

        const size_t first = find_free_chunks(chunks);
        if (first != MAX_SIZE_T)
        {
            void* const address = (unsigned char*)g_heap + first * CHUNK_SIZE;
            void* const committed = VirtualAlloc(
                address, allocation_size, MEM_COMMIT, PAGE_READWRITE);
            if (committed == address)
            {
                memset(g_heap_map + first, 1, chunks);
                g_committed_chunks += chunks;
                if (g_committed_chunks > g_peak_committed_chunks)
                    g_peak_committed_chunks = g_committed_chunks;
                if (first == g_first_free_chunk)
                    refresh_first_free_hint(first + chunks);

                ReleaseSRWLockExclusive(&g_heap_lock);
                return address;
            }

            if (committed)
                VirtualFree(committed, 0, MEM_RELEASE);
        }

        ReleaseSRWLockExclusive(&g_heap_lock);
    }

    return allocate_low_address_fallback(allocation_size);
}

int XR_DESTROY(void* ptr, size_t size)
{
    if (!ptr || ptr == MFAIL || !size || size > MAX_SIZE_T - (CHUNK_SIZE - 1))
        return -1;

    unsigned char* const address = (unsigned char*)ptr;
    const size_t chunks = CHUNKS_FROM_SIZE(size);
    const size_t release_size = chunks * CHUNK_SIZE;

    const uintptr_t allocation_begin = (uintptr_t)address;
    const uintptr_t allocation_end = allocation_begin + release_size;
    const uintptr_t pool_begin = (uintptr_t)g_heap;
    const uintptr_t pool_end = pool_begin + g_chunk_count * CHUNK_SIZE;
    const int belongs_to_pool = g_heap &&
        allocation_begin >= pool_begin && allocation_begin < pool_end;

    if (!belongs_to_pool)
        return release_fallback_range(address, release_size);

    if (allocation_end < allocation_begin || allocation_end > pool_end)
        return -1;

    const size_t offset = (size_t)(allocation_begin - pool_begin);
    if ((offset % CHUNK_SIZE) != 0)
        return -1;

    const size_t first = offset / CHUNK_SIZE;
    if (!chunks || first + chunks > g_chunk_count)
        return -1;

    AcquireSRWLockExclusive(&g_heap_lock);

    for (size_t index = first; index < first + chunks; ++index)
    {
        if (g_heap_map[index] == 0)
        {
            ReleaseSRWLockExclusive(&g_heap_lock);
            return -1;
        }
    }

    if (VirtualFree(address, release_size, MEM_DECOMMIT))
    {
        memset(g_heap_map + first, 0, chunks);
        g_committed_chunks = g_committed_chunks >= chunks ?
            g_committed_chunks - chunks : 0;
        if (first < g_first_free_chunk)
            g_first_free_chunk = first;

        ReleaseSRWLockExclusive(&g_heap_lock);
        return 0;
    }

    ReleaseSRWLockExclusive(&g_heap_lock);
    return -1;
}

void XR_EARLY_INIT(void)
{
    ntavm = (PNTAVM)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                   "NtAllocateVirtualMemory");
    XR_INIT();
}

void XR_GET_POOL_STATS(xr_luajit_pool_stats* stats)
{
    if (!stats)
        return;

    memset(stats, 0, sizeof(*stats));
    stats->requested_bytes = (size_t)REQUESTED_POOL_MB * 1024u * 1024u;

    AcquireSRWLockShared(&g_heap_lock);
    stats->base = g_heap;
    stats->reserved_bytes = g_chunk_count * CHUNK_SIZE;
    stats->committed_bytes = g_committed_chunks * CHUNK_SIZE;
    stats->peak_committed_bytes = g_peak_committed_chunks * CHUNK_SIZE;
    stats->fallback_bytes = g_fallback_bytes;
    stats->peak_fallback_bytes = g_peak_fallback_bytes;
    stats->fallback_active_allocations = g_fallback_active_allocations;
    stats->fallback_total_allocations = g_fallback_total_allocations;
    stats->allocation_failures = g_allocation_failures;
    ReleaseSRWLockShared(&g_heap_lock);
}

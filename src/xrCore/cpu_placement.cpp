#include "stdafx.h"
#pragma hdrstop

#include "cpu_placement.h"

#include <intrin.h>
#include <algorithm>

// Policy (Windows 10/11, one processor group):
// - On every CPU the main thread opts out of power throttling (High QoS).
// - Several last-level caches (multi-CCD Ryzen, X3D): the process default CPU Set is
//   the domain with the largest L3, so the main thread and the pool share one L3. On
//   X3D this is the V-Cache CCD, the one the AMD 3D V-Cache driver keeps unparked for
//   games, so the hint agrees with the driver and Game Mode instead of fighting them.
// - The main thread is placed only when Windows exposes a core ranking: EfficiencyClass
//   (Intel P/E cores) or SchedulingClass (AMD CPPC preferred cores, Intel Turbo Boost
//   Max 3.0). Without one (e.g. CPPC disabled in BIOS) Windows keeps full control.
// - The main thread gets the first logical processor of the best core as its ideal
//   processor. Every other engine thread (pool, driver, audio) is kept off that whole
//   core by the process default CPU Set, so the SMT sibling stays free.
// - Windows picks the core at every wake-up and prefers the best ranked idle core over
//   the ideal processor, so once a second, outside the frame, a monitor reserves the
//   core the main thread actually runs on, once it stays there two windows in a row
//   (each move costs it the warm L1/L2). A core with sustained foreign load (no other
//   engine thread runs on the reserved core, so busy time minus the main thread is
//   exact) is removed from the main thread's CPU Set for a while and Windows moves it
//   elsewhere. Load on other cores is not compared: Windows packs the pool onto the
//   preferred cores, so their busy time cannot be split into engine and foreign work.
namespace cpu_placement
{
namespace
{
using get_system_cpu_sets_fn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
using get_process_cpu_sets_fn = BOOL(WINAPI*)(HANDLE, PULONG, ULONG, PULONG);
using set_process_cpu_sets_fn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
using set_thread_cpu_sets_fn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
using set_thread_information_fn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

// THREAD_POWER_THROTTLING_STATE and ThreadPowerThrottling; the SDK hides them below Windows 8.
struct power_throttling_state
{
    ULONG version;
    ULONG control_mask;
    ULONG state_mask;
};
constexpr int thread_power_throttling = 3;
constexpr ULONG power_throttling_execution_speed = 1;

constexpr u32 max_lps = 64;
constexpr DWORD monitor_period_ms = 1000;
// Foreign load (in logical processors) on the reserved core that, seen in two windows in
// a row, removes the core from the main thread's CPU Set for avoid_windows. The estimate
// is noisy up to ~0.6 for one window; a busy SMT sibling reads 1.0 and more.
constexpr double leave_load = 0.75;
constexpr u32 avoid_windows = 30;
// Share of frames the main thread must start on another core, in two windows in a row,
// before the reservation follows it there.
constexpr double follow_share = 0.6;

struct core_slot
{
    u64 mask; // logical processors of the core
    u32 home; // its first logical processor: the main thread's ideal processor
    u8 rank; // SchedulingClass
    u32 avoid_until; // monitor window
};

set_process_cpu_sets_fn set_process_cpu_sets = nullptr;
set_thread_cpu_sets_fn set_thread_cpu_sets = nullptr;

ULONG cpu_set_ids[max_lps]{};
u64 domain = 0; // where the engine runs
u64 main_allowed = 0; // where the main thread may run: P-cores of the domain on hybrid CPUs
// Main thread candidates, best rank first. A plain array: the monitor may still run while
// static destructors do at exit.
core_slot cores[max_lps]{};
u32 core_count = 0;
u64 main_set = 0; // main_allowed minus avoided cores; the whole domain if all are avoided
u32 main_core = 0; // reserved core
u32 loaded_windows = 0;
u32 follow_core = u32(-1); // core the main thread mostly ran on in the previous window
set_thread_information_fn set_thread_information = nullptr;
HANDLE main_thread = nullptr;
bool main_placed = false; // false when only the cache domain is placed
PROCESSOR_NUMBER original_ideal{};
u32 budget = 0;
bool monitoring = false;
volatile LONG main_hits[max_lps]{};

// Guards the hints below against orchestration_threads racing the monitor.
SRWLOCK lock = SRWLOCK_INIT;
bool planned = false;
bool active = false;

u32 lp_count(u64 mask) { return u32(__popcnt64(mask)); }

ULONG to_ids(u64 mask, ULONG* ids)
{
    ULONG count = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
        if (mask & (1ull << lp))
            ids[count++] = cpu_set_ids[lp];
    return count;
}

void set_main_set(u64 mask)
{
    main_set = mask;
    ULONG ids[max_lps];
    set_thread_cpu_sets(main_thread, ids, to_ids(mask, ids));
}

void place_main(u32 core)
{
    main_core = core;
    loaded_windows = 0;
    follow_core = u32(-1);
    ULONG ids[max_lps];
    set_process_cpu_sets(GetCurrentProcess(), ids, to_ids(domain & ~cores[core].mask, ids));
    PROCESSOR_NUMBER ideal{0, BYTE(cores[core].home), 0};
    SetThreadIdealProcessorEx(main_thread, &ideal, nullptr);
}

// High QoS for the main thread: Windows may lower an idle foreground game to Medium QoS
// (lower clocks, E-cores on hybrid CPUs), e.g. in cutscenes or while reading the PDA.
// Control without state = throttling off; no control = the system decides again.
void set_main_high_qos(bool enable)
{
    power_throttling_state state{1, enable ? power_throttling_execution_speed : 0, 0};
    if (set_thread_information)
        set_thread_information(main_thread, thread_power_throttling, &state, sizeof(state));
}

void apply()
{
    if (main_placed)
    {
        set_main_set(main_set);
        place_main(main_core);
        return;
    }
    ULONG ids[max_lps];
    set_process_cpu_sets(GetCurrentProcess(), ids, to_ids(domain, ids));
}

void clear()
{
    set_process_cpu_sets(GetCurrentProcess(), nullptr, 0);
    if (main_placed)
    {
        set_thread_cpu_sets(main_thread, nullptr, 0);
        SetThreadIdealProcessorEx(main_thread, &original_ideal, nullptr);
    }
}

struct snapshot
{
    ULONG64 idle[max_lps];
    ULONG64 main_cycles;
    u64 tsc;
    LONG hits[max_lps];
};

void take(snapshot& s)
{
    ULONG size = sizeof(s.idle);
    ZeroMemory(s.idle, sizeof(s.idle));
    QueryIdleProcessorCycleTime(&size, s.idle);
    QueryThreadCycleTime(main_thread, &s.main_cycles);
    s.tsc = __rdtsc();
    for (u32 lp = 0; lp < max_lps; ++lp)
        s.hits[lp] = main_hits[lp];
}

// One monitor window, under the lock while the hints are active.
void evaluate(const snapshot& previous, const snapshot& current, u32 window)
{
    LONG hits[max_lps], frames = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
    {
        hits[lp] = current.hits[lp] - previous.hits[lp];
        frames += hits[lp];
    }
    // No frames: loading screen, pause or minimized window, nothing worth placing.
    if (frames <= 0)
        return;

    // Frames the main thread started per core, busy time of the reserved core. Windows
    // accounts idle cycles in TSC ticks, so idle/TSC is the idle share.
    const double ticks = double(current.tsc - previous.tsc);
    double busy = 0;
    LONG reserved_frames = 0, dominant_frames = 0;
    u32 dominant = main_core;
    u64 avoided = 0;
    for (u32 k = 0; k < core_count; ++k)
    {
        LONG core_frames = 0;
        for (u32 lp = 0; lp < max_lps; ++lp)
            if (cores[k].mask & (1ull << lp))
            {
                core_frames += hits[lp];
                if (k == main_core)
                    busy += std::clamp(1.0 - double(current.idle[lp] - previous.idle[lp]) / ticks, 0.0, 1.0);
            }
        if (k == main_core)
            reserved_frames = core_frames;
        if (cores[k].avoid_until > window)
            avoided |= cores[k].mask;
        else if (core_frames > dominant_frames)
        {
            dominant = k;
            dominant_frames = core_frames;
        }
    }

    // The main thread's time on the reserved core is estimated from where its frames started.
    const double main_load = double(current.main_cycles - previous.main_cycles) / ticks;
    const double foreign = std::max(busy - main_load * reserved_frames / frames, 0.0);
    if (foreign < leave_load)
        loaded_windows = 0;
    else if (++loaded_windows >= 2)
    {
        cores[main_core].avoid_until = window + avoid_windows;
        avoided |= cores[main_core].mask;
        Msg("* CPU placement: LP%u core avoided, foreign load %.2f", cores[main_core].home, foreign);
    }

    // With every candidate avoided Windows gets the whole domain, E-cores included on
    // hybrid CPUs, as the last resort.
    const u64 allowed = (main_allowed & ~avoided) ? main_allowed & ~avoided : domain;
    if (allowed != main_set)
        set_main_set(allowed);

    u32 target = main_core;
    if (avoided & cores[main_core].mask)
    {
        for (u32 k = 0; k < core_count; ++k)
            if (!(cores[k].mask & avoided))
            {
                target = k;
                break;
            }
    }
    else
    {
        const bool follow = dominant != main_core && dominant_frames >= follow_share * frames;
        if (follow && follow_core == dominant)
            target = dominant;
        follow_core = follow ? dominant : u32(-1);
    }
    if (target == main_core)
        return;

    Msg("* CPU placement: main thread core LP%u -> LP%u", cores[main_core].home, cores[target].home);
    place_main(target);
}

void monitor(void*)
{
    snapshot previous, current;
    take(previous);
    for (u32 window = 1;; ++window)
    {
        Sleep(monitor_period_ms);
        take(current);
        AcquireSRWLockExclusive(&lock);
        if (active)
            evaluate(previous, current, window);
        ReleaseSRWLockExclusive(&lock);
        previous = current;
    }
}
} // namespace

void initialize()
{
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    set_thread_information = reinterpret_cast<set_thread_information_fn>(GetProcAddress(kernel, "SetThreadInformation"));
    set_main_high_qos(true);
    active = true;

    const auto get_system_cpu_sets = reinterpret_cast<get_system_cpu_sets_fn>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
    const auto get_process_cpu_sets = reinterpret_cast<get_process_cpu_sets_fn>(GetProcAddress(kernel, "GetProcessDefaultCpuSets"));
    set_process_cpu_sets = reinterpret_cast<set_process_cpu_sets_fn>(GetProcAddress(kernel, "SetProcessDefaultCpuSets"));
    set_thread_cpu_sets = reinterpret_cast<set_thread_cpu_sets_fn>(GetProcAddress(kernel, "SetThreadSelectedCpuSets"));
    if (!get_system_cpu_sets || !get_process_cpu_sets || !set_process_cpu_sets || !set_thread_cpu_sets)
    {
        Msg("* CPU placement: off, CPU Sets need Windows 10");
        return;
    }

    // shortcut: one processor group (up to 64 logical processors); larger systems keep the Windows default.
    if (GetActiveProcessorGroupCount() != 1)
    {
        Msg("* CPU placement: off, more than one processor group");
        return;
    }

    // Task Manager, Process Lasso or "start /affinity" already decided for this process.
    DWORD_PTR process_mask = 0, system_mask = 0;
    ULONG preset = 0;
    get_process_cpu_sets(GetCurrentProcess(), nullptr, 0, &preset);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) || process_mask != system_mask || preset)
    {
        Msg("* CPU placement: off, affinity or CPU Sets are set externally");
        return;
    }

    ULONG length = 0;
    get_system_cpu_sets(nullptr, 0, &length, GetCurrentProcess(), 0);
    xr_vector<u8> sets(length);
    if (!length || !get_system_cpu_sets(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data()), length, &length,
        GetCurrentProcess(), 0))
    {
        Msg("* CPU placement: off, no CPU Set information");
        return;
    }

    u8 core_of[max_lps]{}, cache_of[max_lps]{}, efficiency_of[max_lps]{}, rank_of[max_lps]{};
    u64 usable = 0;
    for (u8* it = sets.data(); it < sets.data() + length; it += reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(it)->Size)
    {
        const SYSTEM_CPU_SET_INFORMATION& info = *reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(it);
        // CPU Sets reserved for another application (Core Reservation) are not ours.
        if (info.Type != CpuSetInformation || info.CpuSet.LogicalProcessorIndex >= max_lps ||
            (info.CpuSet.Allocated && !info.CpuSet.AllocatedToTargetProcess))
            continue;
        const u32 lp = info.CpuSet.LogicalProcessorIndex;
        usable |= 1ull << lp;
        cpu_set_ids[lp] = info.CpuSet.Id;
        core_of[lp] = info.CpuSet.CoreIndex;
        cache_of[lp] = info.CpuSet.LastLevelCacheIndex;
        efficiency_of[lp] = info.CpuSet.EfficiencyClass;
        rank_of[lp] = info.CpuSet.SchedulingClass;
    }
    if (!usable)
    {
        Msg("* CPU placement: off, every CPU Set is reserved");
        return;
    }

    // L3 size per logical processor tells an X3D V-Cache CCD from a plain one.
    u32 l3_of[max_lps]{};
    DWORD cache_length = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &cache_length);
    xr_vector<u8> caches(cache_length);
    if (cache_length && GetLogicalProcessorInformationEx(RelationCache,
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(caches.data()), &cache_length))
    {
        for (u8* it = caches.data(); it < caches.data() + cache_length;
            it += reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(it)->Size)
        {
            const CACHE_RELATIONSHIP& cache = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(it)->Cache;
            if (cache.Level == 3)
                for (u32 lp = 0; lp < max_lps; ++lp)
                    if (cache.GroupMask.Mask & (1ull << lp))
                        l3_of[lp] = cache.CacheSize;
        }
    }

    // One cache domain for the whole engine: the largest L3, then the best ranked core,
    // then the larger domain.
    u64 best_domain = 0;
    u32 best_lp = 0;
    u8 best_rank = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
    {
        if (!(usable & (1ull << lp)))
            continue;
        u64 mask = 0;
        u8 rank = 0;
        for (u32 other = 0; other < max_lps; ++other)
            if ((usable & (1ull << other)) && cache_of[other] == cache_of[lp])
            {
                mask |= 1ull << other;
                rank = std::max(rank, rank_of[other]);
            }
        if (!best_domain || l3_of[lp] > l3_of[best_lp] ||
            (l3_of[lp] == l3_of[best_lp] && (rank > best_rank || (rank == best_rank && lp_count(mask) > lp_count(best_domain)))))
        {
            best_domain = mask;
            best_lp = lp;
            best_rank = rank;
        }
    }
    // A small domain (Zen 5c cluster split, LP E-cores) would starve the pool: keep all.
    domain = lp_count(best_domain) * 2 >= lp_count(usable) ? best_domain : usable;

    u8 top_efficiency = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
        if (domain & (1ull << lp))
            top_efficiency = std::max(top_efficiency, efficiency_of[lp]);

    bool hybrid = false;
    u32 domain_cores = 0;
    u64 seen = 0;
    core_count = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
    {
        if (!(domain & (1ull << lp)) || (seen & (1ull << lp)))
            continue;
        u64 mask = 0;
        for (u32 other = lp; other < max_lps; ++other)
            if ((domain & (1ull << other)) && core_of[other] == core_of[lp])
                mask |= 1ull << other;
        seen |= mask;
        ++domain_cores;
        if (efficiency_of[lp] == top_efficiency)
            cores[core_count++] = {mask, lp, rank_of[lp], 0};
        else
            hybrid = true;
    }
    std::stable_sort(cores, cores + core_count, [](const core_slot& a, const core_slot& b) { return a.rank > b.rank; });
    const u8 max_rank = cores[0].rank, min_rank = cores[core_count - 1].rank;

    // Without a ranking from the OS there is no better core to pick; with fewer than
    // four cores a reserved one would starve everything else.
    const bool place_main_thread = (hybrid || max_rank != min_rank) && domain_cores >= 4;
    if (!place_main_thread && domain == usable)
    {
        Msg("* CPU placement: off, one L3 and no preferred-core ranking (CPPC / Turbo Boost Max off?)");
        return;
    }

    if (domain != usable)
        Msg("* CPU placement: engine on LP mask %llX of %llX (L3 %u KB)", domain, usable, l3_of[best_lp] / 1024);

    // The pool keeps one worker per logical processor of the domain, as without placement:
    // its workers just do not run on the main thread's core.
    budget = lp_count(domain);
    planned = true;
    main_placed = place_main_thread;
    if (place_main_thread)
    {
        GetThreadIdealProcessorEx(main_thread, &original_ideal);
        for (u32 k = 0; k < core_count; ++k)
            main_allowed |= cores[k].mask;
        main_set = main_allowed;
    }
    apply();
    if (!place_main_thread)
        return;

    monitoring = true;
    thread_spawn(monitor, "CPU placement monitor", 0, nullptr);
    Msg("* CPU placement: main thread on LP%u (%s), other engine threads off that core", cores[0].home,
        hybrid ? "P-core" : "preferred core");
}

void set_enabled(bool enabled)
{
    AcquireSRWLockExclusive(&lock);
    if (enabled != active)
    {
        active = enabled;
        set_main_high_qos(enabled);
        if (planned)
            enabled ? apply() : clear();
        Msg("* CPU placement: %s", enabled ? "on" : "off, Windows schedules every engine thread");
    }
    ReleaseSRWLockExclusive(&lock);
}

u32 thread_budget() { return budget; }

void note_main_frame()
{
    if (monitoring)
        ++main_hits[GetCurrentProcessorNumber() & (max_lps - 1)];
}
} // namespace cpu_placement

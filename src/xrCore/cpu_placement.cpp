#include "stdafx.h"
#pragma hdrstop

#include "cpu_placement.h"

#include <intrin.h>
#include <algorithm>

// QUARK-SCHEDULER (Windows 10/11, one processor group), the engine's own placement, applied at start:
// - Cores of the game: the process affinity. On a CPU with L3 caches of different sizes (X3D with one V-Cache
//   CCD) the domain with the largest L3, even though CPPC ranks the other CCD higher: every engine, driver and audio
//   thread shares one L3 and never crosses to the other CCD. L3 caches of one size (one L3, a plain two-CCD Ryzen): no
//   binding. The player does not choose the cores of the game. An affinity inherited from the launcher (a child gets
//   its parent's affinity, not its CPU Sets) narrows the choice.
// - Main thread: placed with a core ranking from Windows (SchedulingClass: AMD CPPC preferred cores, Intel Turbo
//   Boost Max 3.0) and on Intel hybrids (EfficiencyClass: P/E cores, the main thread never runs on an E-core), never on
//   an X3D bound above nor without a ranking (e.g. CPPC disabled in BIOS). It gets the first logical processor of the
//   best core as its ideal processor. The process default CPU Set keeps the pool, driver and audio off the whole core.
//   On a homogeneous CPU with one L3, the main thread's CPU Set keeps both SMT siblings of that core for L1/L2 locality.
//   A monitor moves the reservation away from sustained foreign load. On topologies with a wider main-thread CPU Set
//   it can also follow the core the main thread consistently uses. Other cores mix engine and foreign work.
//   Windows never power throttles the main thread.
namespace cpu_placement
{
namespace
{
constexpr u32 max_l3 = 16;
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
set_thread_information_fn set_thread_information = nullptr;
HANDLE main_thread = nullptr;
PROCESSOR_NUMBER original_ideal{};

// Topology, read once by initialize(). Plain arrays: the monitor may still run while static destructors do at exit.
bool topology = false;
ULONG cpu_set_ids[max_lps]{};
u64 core_mask_of[max_lps]{};
u8 rank_of[max_lps]{}, efficiency_of[max_lps]{};
u64 usable = 0; // logical processors not reserved for another application
u64 inherited = 0; // the process affinity at start
bool external = false;
u64 l3_mask[max_l3]{};
u32 l3_kb[max_l3]{};
u32 l3_count = 0, best_l3 = 0;
bool asymmetric_l3 = false; // L3 domains of different known sizes (X3D) and the largest can hold the game
bool l3_unknown = false;
u64 p_cores = 0; // the top efficiency class of a hybrid, 0 otherwise
bool ranked = false;
u32 core_total = 0;
u32 budget = 0;

// The plan in force, under the lock.
SRWLOCK lock = SRWLOCK_INIT;
u64 bind = 0; // process affinity set by the scheduler, 0: the inherited one
u64 game = 0; // cores of the game
core_slot cores[max_lps]{}; // main thread candidates in game, best rank first
u32 core_count = 0;
bool auto_main = false;
int main_home = -1;
u64 main_allowed = 0; // where the main thread may run in AUTO: the candidates
u64 main_set = 0; // last CPU Set Windows accepted
u32 main_core = 0; // reserved core
u32 loaded_windows = 0;
u32 follow_core = u32(-1); // core the main thread mostly ran on in the previous window
bool monitoring = false;
volatile LONG main_hits[max_lps]{};

u32 lp_count(u64 mask) { return u32(__popcnt64(mask)); }

ULONG to_ids(u64 mask, ULONG* ids)
{
    ULONG count = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
        if (mask & (1ull << lp))
            ids[count++] = cpu_set_ids[lp];
    return count;
}

bool set_main_set(u64 mask)
{
    ULONG ids[max_lps];
    if (!set_thread_cpu_sets(main_thread, ids, to_ids(mask, ids)))
        return false;
    main_set = mask;
    return true;
}

bool update_main_set(u32 core, u64 available)
{
    // If all candidates were avoided, let Windows choose from the original candidates.
    const u64 allowed = available ? available : main_allowed;
    const u64 local = available && l3_count == 1 && !p_cores ? cores[core].mask : allowed;
    if (local == main_set || set_main_set(local))
        return true;
    return local != allowed && (allowed == main_set || set_main_set(allowed));
}

// The other engine threads keep off the main thread's core.
void reserve(u64 core_mask)
{
    ULONG ids[max_lps];
    set_process_cpu_sets(GetCurrentProcess(), ids, to_ids(game & ~core_mask, ids));
}

void place_main(u32 core)
{
    main_core = core;
    main_home = int(cores[core].home);
    loaded_windows = 0;
    follow_core = u32(-1);
    reserve(cores[core].mask);
    PROCESSOR_NUMBER ideal{0, BYTE(cores[core].home), 0};
    SetThreadIdealProcessorEx(main_thread, &ideal, nullptr);
}

// High QoS for the main thread: Windows may lower an idle foreground game to Medium QoS
// (lower clocks, E-cores on hybrid CPUs), e.g. in cutscenes or while reading the PDA.
// Control without state = throttling off.
void set_main_high_qos()
{
    power_throttling_state state{1, power_throttling_execution_speed, 0};
    if (set_thread_information)
        set_thread_information(main_thread, thread_power_throttling, &state, sizeof(state));
}

// Cores of the game, always the engine's choice: the largest L3 of an X3D; 0: not bound.
u64 game_cores() { return asymmetric_l3 ? l3_mask[best_l3] & inherited : 0; }

// The main thread's candidates: the cores of the top efficiency class in game, best rank first.
u32 collect_cores(bool& hybrid_game)
{
    u8 top = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
        if (game & (1ull << lp))
            top = std::max(top, efficiency_of[lp]);
    hybrid_game = false;
    core_count = 0;
    u32 all_cores = 0;
    u64 seen = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
    {
        if (!(game & (1ull << lp)) || (seen & (1ull << lp)))
            continue;
        const u64 mask = core_mask_of[lp] & game;
        seen |= mask;
        ++all_cores;
        if (efficiency_of[lp] == top)
            cores[core_count++] = {mask, lp, rank_of[lp], 0};
        else
            hybrid_game = true;
    }
    std::stable_sort(cores, cores + core_count, [](const core_slot& a, const core_slot& b) { return a.rank > b.rank; });
    return all_cores;
}

void apply_locked()
{
    if (!topology)
        return;

    // Setting the process affinity resets the affinity of every thread, the main thread's lock included.
    const u64 next_bind = game_cores();
    if (next_bind != bind)
    {
        if (SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(next_bind ? next_bind : inherited)))
        {
            bind = next_bind;
            if (bind)
                Msg("* CPU placement: game bound to LP mask %llX of %llX", bind, usable);
            else
                Msg("* CPU placement: game on every logical processor it may use (%llX)", inherited);
        }
        else
            Msg("! CPU placement: affinity %llX refused, error %u", next_bind ? next_bind : inherited, GetLastError());
    }
    game = (bind ? bind : inherited) & usable;

    bool hybrid_game;
    const u32 game_cores_count = collect_cores(hybrid_game);
    set_process_cpu_sets(GetCurrentProcess(), nullptr, 0);
    if (set_thread_cpu_sets(main_thread, nullptr, 0))
        main_set = 0;
    SetThreadAffinityMask(main_thread, DWORD_PTR(game));
    SetThreadIdealProcessorEx(main_thread, &original_ideal, nullptr);
    const int previous_home = main_home;
    const bool previous_auto = auto_main;
    auto_main = false;
    main_home = -1;
    main_allowed = 0;
    if (!core_count)
        return;

    // CPPC ranks unless the game is bound to an X3D's CCD, P/E cores on any hybrid; an external affinity or CPU Set
    // already decided. With fewer than four cores a reserved one would starve everything else.
    auto_main = !external && game_cores_count >= 4 &&
        (hybrid_game || (!asymmetric_l3 && !l3_unknown && cores[0].rank != cores[core_count - 1].rank));
    if (auto_main)
    {
        for (u32 k = 0; k < core_count; ++k)
            main_allowed |= cores[k].mask;
        if (!update_main_set(0, main_allowed))
        {
            auto_main = false;
            Msg("! CPU placement: Windows rejected the main thread CPU Sets");
            return;
        }
        place_main(0);
        if (!previous_auto || previous_home != main_home)
            Msg("* CPU placement: main thread on LP%u (%s), other engine threads off that core", cores[0].home,
                hybrid_game ? "P-core" : "preferred core");
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

// One monitor window, under the lock while AUTO places the main thread.
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
    if (foreign < leave_load || (avoided & cores[main_core].mask))
        loaded_windows = 0;
    else if (++loaded_windows >= 2)
    {
        cores[main_core].avoid_until = window + avoid_windows;
        avoided |= cores[main_core].mask;
        Msg("* CPU placement: LP%u core avoided, foreign load %.2f", cores[main_core].home, foreign);
    }

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
    if (update_main_set(target, main_allowed & ~avoided) && target != main_core)
    {
        Msg("* CPU placement: main thread core LP%u -> LP%u", cores[main_core].home, cores[target].home);
        place_main(target);
    }
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
        if (auto_main)
            evaluate(previous, current, window);
        ReleaseSRWLockExclusive(&lock);
        previous = current;
    }
}

// Reads the topology; false when Windows does not give it.
bool read_topology()
{
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto get_system_cpu_sets = reinterpret_cast<get_system_cpu_sets_fn>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
    const auto get_process_cpu_sets = reinterpret_cast<get_process_cpu_sets_fn>(GetProcAddress(kernel, "GetProcessDefaultCpuSets"));
    set_process_cpu_sets = reinterpret_cast<set_process_cpu_sets_fn>(GetProcAddress(kernel, "SetProcessDefaultCpuSets"));
    set_thread_cpu_sets = reinterpret_cast<set_thread_cpu_sets_fn>(GetProcAddress(kernel, "SetThreadSelectedCpuSets"));
    if (!get_system_cpu_sets || !get_process_cpu_sets || !set_process_cpu_sets || !set_thread_cpu_sets)
    {
        Msg("* CPU placement: off, CPU Sets need Windows 10");
        return false;
    }

    // shortcut: one processor group (up to 64 logical processors); larger systems keep the Windows default.
    if (GetActiveProcessorGroupCount() != 1)
    {
        Msg("* CPU placement: off, more than one processor group");
        return false;
    }

    // Task Manager, Process Lasso, "start /affinity" or the launcher already decided for this process.
    DWORD_PTR process_mask = 0, system_mask = 0;
    ULONG preset = 0;
    get_process_cpu_sets(GetCurrentProcess(), nullptr, 0, &preset);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
    {
        Msg("* CPU placement: off, no process affinity");
        return false;
    }
    inherited = process_mask;
    external = process_mask != system_mask || preset;

    ULONG length = 0;
    get_system_cpu_sets(nullptr, 0, &length, GetCurrentProcess(), 0);
    xr_vector<u8> sets(length);
    if (!length || !get_system_cpu_sets(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data()), length, &length,
        GetCurrentProcess(), 0))
    {
        Msg("* CPU placement: off, no CPU Set information");
        return false;
    }

    u8 core_of[max_lps]{}, cache_of[max_lps]{};
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
    if (!usable || !(usable & inherited))
    {
        Msg("* CPU placement: off, every CPU Set is reserved");
        return false;
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

    // Cores (numbered like the THREADS tab), L3 domains in the order of their first logical processor, the ranking.
    u8 top_efficiency = 0, low_efficiency = 255, max_rank = 0, min_rank = 255;
    u32 numbers = 0;
    for (u32 lp = 0; lp < max_lps; ++lp)
    {
        if (!(usable & (1ull << lp)))
            continue;
        top_efficiency = std::max(top_efficiency, efficiency_of[lp]);
        low_efficiency = std::min(low_efficiency, efficiency_of[lp]);
        max_rank = std::max(max_rank, rank_of[lp]);
        min_rank = std::min(min_rank, rank_of[lp]);
        u64 core = 0, domain = 0;
        for (u32 other = 0; other < max_lps; ++other)
            if (usable & (1ull << other))
            {
                core |= core_of[other] == core_of[lp] ? 1ull << other : 0;
                domain |= cache_of[other] == cache_of[lp] ? 1ull << other : 0;
            }
        core_mask_of[lp] = core;
        numbers += core & ((1ull << lp) - 1) ? 0 : 1; // lp is the first processor of its core
        u32 k = 0;
        while (k < l3_count && l3_mask[k] != domain)
            ++k;
        if (k == l3_count && l3_count < max_l3)
        {
            l3_mask[l3_count] = domain;
            l3_kb[l3_count++] = l3_of[lp] / 1024;
        }
    }
    ranked = max_rank != min_rank;
    core_total = numbers;
    if (top_efficiency != low_efficiency)
        for (u32 lp = 0; lp < max_lps; ++lp)
            if ((usable & (1ull << lp)) && efficiency_of[lp] == top_efficiency)
                p_cores |= 1ull << lp;

    // The game's L3 domain: the largest L3, then the best ranked core, then the larger domain.
    auto rank = [](u64 mask) {
        u8 best = 0;
        for (u32 lp = 0; lp < max_lps; ++lp)
            if (mask & (1ull << lp))
                best = std::max(best, rank_of[lp]);
        return best;
    };
    for (u32 k = 1; k < l3_count; ++k)
    {
        const u32 a = l3_kb[k], b = l3_kb[best_l3];
        if (a > b || (a == b && (rank(l3_mask[k]) > rank(l3_mask[best_l3]) ||
            (rank(l3_mask[k]) == rank(l3_mask[best_l3]) && lp_count(l3_mask[k]) > lp_count(l3_mask[best_l3])))))
            best_l3 = k;
    }
    // Without sizes the V-Cache CCD cannot be told from the other one; a small domain (Zen 5c cluster split) would
    // starve the pool: both stay as one L3.
    l3_unknown = l3_count > 1 && !l3_kb[best_l3];
    bool sizes_differ = false;
    for (u32 k = 0; k < l3_count; ++k)
        sizes_differ |= l3_kb[k] != l3_kb[best_l3];
    asymmetric_l3 = sizes_differ && !l3_unknown && lp_count(l3_mask[best_l3]) * 2 >= lp_count(usable);
    if (l3_unknown)
        Msg("* CPU placement: several L3 caches of unknown size, the game is not bound");
    return true;
}
} // namespace

void initialize()
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    set_thread_information = reinterpret_cast<set_thread_information_fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation"));
    GetThreadIdealProcessorEx(main_thread, &original_ideal);
    set_main_high_qos();
    topology = read_topology();
    // The pool keeps one worker per logical processor of the AUTO cores of the game: when the main thread is placed
    // its workers just do not run on the main thread's core.
    if (topology)
        budget = lp_count(game_cores());

    AcquireSRWLockExclusive(&lock);
    apply_locked();
    ReleaseSRWLockExclusive(&lock);
    if (topology && !auto_main && !bind && !main_allowed)
        Msg(external ? "* CPU placement: AUTO leaves the threads to Windows, affinity or CPU Sets are set externally"
                     : "* CPU placement: AUTO leaves the threads to Windows, one L3 and no preferred-core ranking");
    // The monitor exists where AUTO may place the main thread.
    if (topology && !external && core_total >= 4 && (p_cores || (ranked && !asymmetric_l3 && !l3_unknown)))
    {
        monitoring = true;
        thread_spawn(monitor, "CPU placement monitor", 0, nullptr);
    }
}

u32 thread_budget() { return budget; }

void note_main_frame()
{
    if (monitoring)
        ++main_hits[GetCurrentProcessorNumber() & (max_lps - 1)];
}
} // namespace cpu_placement

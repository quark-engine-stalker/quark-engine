#pragma once

// RAM-aware performance policy. The policy only changes cache/prewarm budgets;
// it never changes gameplay, script callback order, resource names or file data.
namespace xr_memory_policy
{
enum class tier : u32
{
    constrained = 0,
    balanced,
    performance,
    abundant
};

struct snapshot
{
    u64 installed_mb = 0;
    u64 usable_mb = 0;
    u64 available_mb = 0;
    u32 load_percent = 0;
    tier mode = tier::balanced;
};

inline snapshot query()
{
    snapshot result;
#ifdef _WIN32
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
    {
        result.usable_mb = status.ullTotalPhys / (1024ull * 1024ull);
        result.available_mb = status.ullAvailPhys / (1024ull * 1024ull);
        result.load_percent = status.dwMemoryLoad;
    }

    // Use the physically-installed amount when Windows exposes it, but resolve
    // dynamically so older SDK target macros cannot break the build.
    using get_installed_memory_fn = BOOL (WINAPI*)(PULONGLONG);
    const HMODULE kernel = GetModuleHandleA("kernel32.dll");
    const auto get_installed_memory = kernel ? reinterpret_cast<get_installed_memory_fn>(
        GetProcAddress(kernel, "GetPhysicallyInstalledSystemMemory")) : nullptr;
    ULONGLONG installed_kb = 0;
    if (get_installed_memory && get_installed_memory(&installed_kb))
        result.installed_mb = installed_kb / 1024ull;
#endif

    if (!result.installed_mb)
        result.installed_mb = result.usable_mb;
    if (!result.usable_mb)
        result.usable_mb = result.installed_mb;

    const u64 physical_mb = result.installed_mb ? result.installed_mb : result.usable_mb;

    // Available RAM can dynamically downgrade an otherwise large-memory system.
    // This prevents aggressive prewarm when another application already owns the
    // spare memory or when a very large mod setup has consumed the headroom.
    if (physical_mb <= 10ull * 1024ull || (result.available_mb && result.available_mb < 2ull * 1024ull))
        result.mode = tier::constrained;
    else if (physical_mb < 24ull * 1024ull || (result.available_mb && result.available_mb < 8ull * 1024ull))
        result.mode = tier::balanced;
    else if (physical_mb >= 48ull * 1024ull && (!result.available_mb || result.available_mb >= 16ull * 1024ull))
        result.mode = tier::abundant;
    else
        result.mode = tier::performance;

    return result;
}

inline LPCSTR tier_name(const tier value)
{
    switch (value)
    {
    case tier::constrained: return "constrained";
    case tier::balanced: return "balanced";
    case tier::performance: return "performance";
    case tier::abundant: return "abundant";
    default: return "unknown";
    }
}

inline u32 sound_cache_mb(const snapshot& value)
{
    switch (value.mode)
    {
    case tier::constrained: return 128;
    case tier::balanced: return 256;
    case tier::performance: return 384;
    case tier::abundant: return 512;
    default: return 256;
    }
}

inline bool full_sound_source_prewarm(const snapshot& value)
{
    // Full source prewarm is intentionally reserved for systems with both a large
    // physical-RAM pool and real free headroom at startup.
    if (value.mode < tier::performance)
        return false;
    return !value.available_mb || value.available_mb >= 10ull * 1024ull;
}

inline u64 sound_prewarm_reserve_mb(const snapshot& value)
{
    switch (value.mode)
    {
    case tier::performance: return 6ull * 1024ull;
    case tier::abundant: return 8ull * 1024ull;
    default: return 0;
    }
}

inline u32 texture_warmup_max(const snapshot& value)
{
    switch (value.mode)
    {
    case tier::constrained: return 512;
    case tier::balanced: return 1024;
    case tier::performance: return 2048;
    case tier::abundant: return 4096;
    default: return 1024;
    }
}

inline u32 texture_warmup_slow_ms(const snapshot& value)
{
    // Keep low-memory systems conservative. On systems with sufficient RAM,
    // retaining UI textures that cost 2-3 ms to decode/upload trades a bounded
    // amount of memory for noticeably smoother first-open inventory/menu frames.
    switch (value.mode)
    {
    case tier::constrained: return 4;
    case tier::balanced: return 3;
    case tier::performance:
    case tier::abundant: return 2;
    default: return 3;
    }
}

inline u32 model_manifest_max(const snapshot& value)
{
    switch (value.mode)
    {
    case tier::constrained: return 2048;
    case tier::balanced: return 4096;
    case tier::performance: return 8192;
    case tier::abundant: return 12288;
    default: return 4096;
    }
}

inline u32 model_prefetch_inflight(const snapshot& value)
{
    switch (value.mode)
    {
    case tier::constrained: return 32;
    case tier::balanced: return 64;
    case tier::performance: return 96;
    case tier::abundant: return 128;
    default: return 64;
    }
}
}

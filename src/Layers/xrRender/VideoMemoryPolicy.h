#pragma once

// Optional cache ownership only. DXGI's per-process budget is not the card's
// advertised capacity, and may change while the application is running.
namespace video_memory_policy
{
constexpr u32 warm_idle_ms = 5u * 60u * 1000u;
constexpr u32 warm_pressure_idle_ms = 60u * 1000u;
constexpr u32 warm_hard_pressure_idle_ms = 30u * 1000u;

inline u64 optional_texture_budget(u64 configured, u64 budget, u64 usage, u64 warm_resident)
{
    if (!budget)
        return configured; // Legacy fallback when DXGI budget reporting is unavailable.

    const u64 target_usage = budget - budget / 8u;
    const u64 required_usage = usage - _min(usage, warm_resident);
    const u64 headroom = required_usage < target_usage ? target_usage - required_usage : 0;
    // Preserve the existing 256 MiB default on an 8 GiB card with the smaller
    // real DXGI budget. The headroom check still contracts it under pressure.
    return _min(configured, _min(budget / 24u, headroom));
}

inline u32 warm_age(u32 now, u32 retained_at, u32 last_used)
{
    // Unsigned subtraction also handles the engine millisecond clock wrapping.
    // A never-bound texture still gets a full grace interval after preparation.
    return _min(now - retained_at, now - last_used);
}
} // namespace video_memory_policy

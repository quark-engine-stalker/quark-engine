#pragma once

// Soft placement of engine threads on the CPU topology: CPU Sets and the ideal
// processor only, never affinity masks. The policy is described in cpu_placement.cpp.
namespace cpu_placement
{
// Main thread, before the worker pool and auxiliary threads are created.
XRCORE_API void initialize();
// orchestration_threads on|off: applies or removes every hint at runtime.
XRCORE_API void set_enabled(bool enabled);
// Logical processors for the main thread plus pool workers; 0 when there is no plan.
XRCORE_API u32 thread_budget();
// Main thread, once per rendered frame: records where the main thread runs.
XRCORE_API void note_main_frame();
} // namespace cpu_placement

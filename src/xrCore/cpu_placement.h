#pragma once

// QUARK-SCHEDULER: where the threads of the game run, decided by the engine alone. The policy is described in
// cpu_placement.cpp.
namespace cpu_placement
{
// Main thread, before the worker pool and auxiliary threads are created.
XRCORE_API void initialize();
// Logical processors for the main thread plus pool workers; 0 when the game is not bound.
XRCORE_API u32 thread_budget();
// Main thread, once per rendered frame: records where the main thread runs.
XRCORE_API void note_main_frame();
} // namespace cpu_placement

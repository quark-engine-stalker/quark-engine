# Changelog

**English** · [Русский](CHANGELOG.ru.md)

## VER 0.0.4 == [BUG-FIX]

### FIXES

- The test warm‑up of the inventory UI when loading a save has been removed
- The engine crash when throwing `M24 SWS` and `decor_poster5` from the inventory has been fixed
- Corrections have been made to the detected item physics issues

## 0.0.3 == [OPEN BETA]

### OPTIMIZATIONS

- Reduced repeated checks of nearby cover points.
- Optimized cover occupancy and danger zone checks.
- Reduced repeated distance and position calculations in combat AI logic.
- Increased `scheduler_batch_size` from 128 to 512.
- Increased the main-thread scheduler queue reserve from 512 to 4096.
- Switched the class predictor to a more stable `EWMA`, reducing the impact of individual spikes on predictions for an entire section.
- Reduced the buildup of `pending/due` scheduled objects during heavy NPC activity.
- Removed repeated material calculations for dynamic opaque shadow casters.
- Removed repeated material constant lookups when rendering NPCs and other dynamic objects in opaque shadow passes.
- Removed a redundant world-matrix update before shadow rendering.
- Removed redundant preparation of an intermediate matrix when rendering NPC parts attached to the same bone in main and shadow passes.
- Removed unnecessary rendering of objects from large destruction queues in main geometry, local shadows and sun shadows.
- The snapshot of a normal object destruction batch is now stored on the stack without an additional memory allocation.

### FIXES

- Fixed a `pure virtual function call` crash in `seqRender` when a UI ComboBox was destroyed between Update and Render.
- Fixed an `integer divide by zero` crash in skeletal animation calculations when a motion had zero sampled keys.
- Fixed the cause of a large number of false `predicted_heavy` detections and the resulting scheduler deferrals.
- Objects marked for deletion are no longer retained in main and shadow render passes until batch deletion is completed.
- Fixed the use of a stale pointer to the deletion queue when objects were added from callbacks.
- New deletion requests are no longer lost after the current batch finishes processing.
- Fixed a race condition between scheduled-object registration and removal that could result in access to an already deleted object.
- Fixed a DX11 crash when starting light visibility checks.
- Fixed handling of stale GPU queries after a renderer reset.
- Fixed a race condition when cancelling light visibility checks from worker threads.
- Fixed cleanup of GPU queries for shadow-casting objects when a light source is modified or removed.
- Removed a redundant firing-distance calculation within a single check.
- Fixed and optimized the cache used for checking objects in an NPC's path.

## 0.0.2 == [CLOSED BETA]

### OPTIMIZATIONS

- Optimized dynamic object visibility.
- Optimized sun shadow cascades and shadow preparation for dynamic lights.
- Optimized object deletion.
- Optimized bulk object spawning and reduced potential stalls, most noticeable when spawning many NPCs.
- Optimized NPC memory processing, enemy searches and group AI checks.
- Optimized the use of previously calculated data: the engine reuses some results.
- Optimized sound occlusion calculations.
- Optimized the startup and buffering of multiple sounds.
- Reduced unnecessary repeated checks and object lookups.
- Slightly reduced rendering overhead when many NPCs and mutants are in the camera's viewing direction.
- Partially removed duplicate and repeated stalker and mutant NPC / AI work that added overhead to the main thread.
- Other minor memory and cache optimizations.
- Added an adaptive caching system to reduce stalls. **[TESTED]**

  | RAM | Mode |
  | :--- | :--- |
  | **32+ GB** | The engine uses memory more actively. Sounds, models and some resources are prepared in advance and remain cached longer. |
  | **16 GB** | Balanced mode: caches are increased moderately. |
  | **8 GB** | Memory-saving mode. GAMMA may experience stalls due to the large volume of mods and scripts. |

- Parallelized NPC vision preparation.

### FIXES

- Fixed recovery after losing focus: repeated Alt + Tab could freeze the image while sound continued playing.
- Fixed a crash when unloading levels without grass details.
- Fixed some stalls when NPCs died.
- Fixed numerous crashes found during testing.
- Fixed local light rendering.

## 0.0.1 == [CLOSED BETA]

### OPTIMIZATIONS AND FIXES

- Removed DX8/DX9 code and support. Only DX11-AVX2 is supported.
- Updated LuaJIT from 2.0.4 to a newer branch of version 2.0.
- Optimized GC / Lua processing.
- Optimized lighting and shadows.
- Reduced unnecessary CPU pipeline overhead that affected performance.
- Optimized the main thread and parts of multithreaded synchronization.
- Optimized memory handling and allocations.
- Improved save and level loading speed.
- Optimized mathematical and geometric calculations using AVX2 where appropriate.
- Optimized NPC skeletons and animations.
- Fixed identified memory leaks and data races.
- Other minor optimizations and removal of obsolete code.

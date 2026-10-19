# Changelog

**English** · [Русский](CHANGELOG.ru.md)

VER 0.0.6 [BUG FIXES]

FIXES:

- Fixed ray query lock release when exceptions occur.
- Fixed COM reference leaks in HDAO that kept render targets and the depth buffer alive after their owners were released.
- Fixed a sound object leak when resetting and destroying the trader animation manager.
- Fixed an artifact status string leak in the item information window. Added safe cleanup when XML initialization is incomplete.
- Fixed preloaded textures being retained without an owner.
- Fixed GPU resource leaks during shutdown in the legacy DXGI mode.
- Fixed MSAA and UAV render target restoration after reset.
- Fixed resource release before swap chain recreation.
- Improved repeated cleanup of the 3D Fluid renderer.
- Fixed memory counter overflows and render target size accounting.
- Fixed memory leaks caused by re-registering script callbacks.
- Fixed callback removal by method name and across Lua coroutines.
- Fixed C++ object cleanup when a Lua finalizer fails.
- Fixed the lifetime management of strings and script attachment windows.
- Fixed callback removal while callbacks are being executed.
- Fixed an engine crash when saving during a level transition.
- Fixed a crash during game shutdown in the `CRender` renderer destructor.
- Fixed several crashes occurring during save loading.
- Various other minor fixes.

VER 0.0.5 [AKA MAJOR UPDATE]

OPTIMIZATIONS:

- Optimized NPC memory object lookups using a shared group cache.
- Reduced repeated full scans for objects missing from NPC memory.
- Eliminated full shared-cache invalidation when replacing a single memory entry.
- Reduced redundant memory data preparation within NPC groups.
- Reduced redundant calls and CPU overhead when processing Lua GC objects.
- Optimized batch processing of Lua GC objects with execution time budgeting.
- Parallelized sorting of large render graphs for main geometry and shadows.
- Eliminated unnecessary previous-frame matrix calculations for skeletal objects in rendering passes where shaders do not use them.
- Small batches are processed sequentially, avoiding unnecessary task creation and queue waits when the worker pool is busy.
- Optimized the transfer of positions and other vectors from C++ to Lua.
- Reduced luabind overhead when handling NPC sound events and position queries.
- Reworked the sound cache to allocate memory on demand instead of preallocating the entire cache at startup.
- Limited unnecessary preloading of models, animations, and textures.
- Limited additional retention of preloaded UI textures.
- Improved cleanup of unused models and animation data when unloading levels.
- Optimized loading of 24-bit DDS textures by eliminating redundant intermediate pixel conversions and temporary buffer initialization.
- Accelerated patrol path loading during savegame loading and level transitions, partly by eliminating redundant data moves when building the path registry.
- Accelerated Lua string pattern processing during XML parsing.
- Eliminated redundant texture creation and copying operations on first use.
- Added early asynchronous preparation of PDA and selected HUD item resources before their first appearance, while respecting the shared preload budget.
- Prepared HUD data is now retained after level loading until it is used or the level is unloaded.
- Optimized CPU-side grass preparation in main rendering and shadow passes, reducing redundant classification and repacking of unchanged data across frames and rendering passes.
- Limited the prepared grass cache by memory usage and the amount of new work performed per frame.
- Optimized HUD material lookups for NPC items, eliminating unnecessary animation and GPU resource loading for supported models.
- Optimized NPC sound event handling by eliminating repeated Lua constant lookups.
- Reduced redundant object type checks in AI and gameplay queries, including an unnecessary check when determining an object's relevance to NPC vision.
- Optimized matrix and vector writes in the DX11 renderer.
- Accelerated resource indexing during engine startup and savegame loading.
- Limited the startup metadata cache by memory usage and ensured it is released before initialization completes.
- Reduced redundant validation of cached shader packets during DSGraph construction.
- Optimized large and ordered DSGraph queues while preserving rendering order.
- Eliminated redundant matrix calculations during sun cascade preparation.
- Moved the primary sun shadow sector lookup to the level-loading stage.
- Eliminated redundant clearing of sun shadow working data.
- Reduced unnecessary memory writes when expanding rendering containers.
- The preloaded texture cache now accounts for the available VRAM budget and the estimated video memory footprint of GPU textures.
- Added selective eviction of long-unused textures retained exclusively by the cache.
- Optional texture preloading is now suspended under VRAM pressure, while required texture loading remains unaffected.
- Eliminated repeated full scans of Lua globals when looking up engine callbacks.
- Accelerated callback checks when removing items from dead NPC inventories.

FIXES:

- Fixed redundant empty Lua GC passes after the atomic phase that unnecessarily consumed frame time.
- Fixed worker task waits that could unnecessarily stall the main thread.
- Fixed the retention of unused base models from automatic preloading.
- Fixed sound cache initialization with large memory limits.
- Fixed the shutdown order of background resource preparation before renderer reset and destruction.
- Fixed early HUD requests being dropped when the background model queue was full by reserving queue slots for selected items.
- Fixed retry handling after a background resource preparation request fails.
- Fixed a native crash in `game_object:name()` during hit processing.
- Fixed sun shadow working buffers remaining allocated after level unloading.
- Fixed COM reference leaks involving GPU resources in post-processing and MSAA resolve passes.
- Fixed the release of a temporary COM reference to the 3D scope depth buffer.
- Fixed a COM reference leak involving the source GPU resource during screenshot export.
- Improved synchronization of texture acquisition and release during concurrent worker requests and cache eviction.
- Added input bounds checks when decompressing corrupted or incomplete compressed data, including savegames.
- Reworked the engine's error logger, upgrading it to v2.0.
- Fixed numerous other minor memory leaks.

PORTED AND ADAPTED FIXES:

- Fixed memory corruption when entering text through Lua ImGui.
  Ported from Xray-Monolith, PR #694:  
  https://github.com/themrdemonized/xray-monolith/pull/694
- Fixed an out-of-bounds buffer access in ImGui text fields with hint text.
  Ported from Dear ImGui, commit 5dd8408:  
  https://github.com/ocornut/imgui/commit/5dd8408
- Fixed crashes when no matching overload exists for Lua functions or methods.
  Ported the defensive portion of the fix from Xray-Monolith, PR #674:  
  https://github.com/themrdemonized/xray-monolith/pull/674
- Fixed cleanup of models used by scripted HUD animations.
  Ported and adapted from Xray-Monolith, PR #726:  
  https://github.com/themrdemonized/xray-monolith/pull/726
- Fixed an issue when detaching launched rockets.
  Ported from Xray-Monolith, PR #713:  
  https://github.com/themrdemonized/xray-monolith/pull/713
- Fixed incorrect movement point lookup results for Chimeras and Lurkers when no suitable point is found.
  Ported from Xray-Monolith, PR #710:  
  https://github.com/themrdemonized/xray-monolith/pull/710
- Fixed incorrect misfire state reset when NPCs reload automatic shotguns.
  Ported from Xray-Monolith, PR #717:  
  https://github.com/themrdemonized/xray-monolith/pull/717

VER 0.0.4 [BUG-FIX]

FIXES:

- The test warm‑up of the inventory UI when loading a save has been removed
- The engine crash when throwing `M24 SWS` and `decor_poster5` from the inventory has been fixed
- Corrections have been made to the detected item physics issues
- The movement order of the initial spawn has been fixed when loading a single‑player game

VER 0.0.3 [OPEN BETA]

OPTIMIZATIONS:

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

FIXES:

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

VER 0.0.2 [CLOSED BETA]

OPTIMIZATIONS:

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

FIXES:

- Fixed recovery after losing focus: repeated Alt + Tab could freeze the image while sound continued playing.
- Fixed a crash when unloading levels without grass details.
- Fixed some stalls when NPCs died.
- Fixed numerous crashes found during testing.
- Fixed local light rendering.

VER 0.0.1 [CLOSED BETA]

OPTIMIZATIONS AND FIXES:

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

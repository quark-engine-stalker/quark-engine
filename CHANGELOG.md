# Changelog

**English** & [Русский](CHANGELOG.ru.md)

VER 1.0.1 [BUG FIXES AND PERFORMANCE]

FIXES:

- Fixed a crash shortly after loading a save near artifacts
- Fixed a crash near bloodsuckers and other mutants when the game runs out of video memory
- Fixed a crash when loading a save near a campfire (the root of the problem still lies in the GAMMA Cooking Overhaul mod; the fixes only cover the engine side)
- Fixed crashes when an NPC exits combat state
- Fixed a crash in combat with zombified stalkers (scripts that mistakenly pass an empty value instead of a position or another object no longer crash the game)
- Fixed crashes in the Zone's life simulation (A-Life) when an NPC, monster, or squad disappears
- Fixed a crash at the end of animations with a camera effect (drinking Anabiotic during an emission sometimes caused a crash)
- Fixed full-screen "gibberish characters" while loading a save (with the RTSS/MSI Afterburner overlay enabled, its font was drawn instead of the loading background)

OPTIMIZATION:

- Texture warm-up cache increased from 256 to 512 MB (half for UI, half for models). If video memory is insufficient, the cache shrinks and is freed.
- Sped up script callbacks (gamedata -> axr_main.script): the subscriber list is no longer re-sorted on every call and is taken from a cache instead.
- Faster XML processing in DXML (gamedata -> dxml_core.script). XML files are no longer parsed and rebuilt in Lua if no mod modifies them. In synthetic tests on 142 real XML files from GAMMA (before: 681 ms -> after: 2 ms)
- Sped up the weapon scope check (gamedata -> aaaa_script_fixes_mp.script). (before: up to 62 µs -> after: up to 3.3 µs)
- Sped up the IsWeapon and IsLauncher checks (gamedata -> _g_patches.script). For items that are not weapons, there is one fewer engine call and no memory allocation per call.
- Reduced the load from monsters' per-frame anomaly traversal (gamedata -> aaaa_script_fixes_mp.script). In a test with 60 anomalies and 30 monsters (before: 164 -> after: 110 engine calls per frame)

PORTED AND ADAPTED FIXES:

- Fixed memory corruption and graphical artifacts when a shader declares an engine constant as a single number (weapon blur in Screen Space Shaders).
  Part of the fix ported from Xray-Monolith, PR #736:
  https://github.com/themrdemonized/xray-monolith/pull/736
- Fixed lightning flash accumulating in the sky color while the game is paused.
  Ported from Xray-Monolith, PR #733:
  https://github.com/themrdemonized/xray-monolith/pull/733
- Fixed a UI element's texture color being reset when its color animation is removed.
  Ported from Xray-Monolith, PR #682:
  https://github.com/themrdemonized/xray-monolith/pull/682
- Fixed small changes to a script light source's range being ignored.
  Part of the fix ported from Xray-Monolith, PR #674:
  https://github.com/themrdemonized/xray-monolith/pull/674
- Weapons dropped by NPCs no longer fly off in the direction of the player's gaze (fixed a crash when no actor is present).
  Ported and adapted from Xray-Monolith, commit 6eefc64:
  https://github.com/themrdemonized/xray-monolith/commit/6eefc64
- Added Tracy 0.14.1 (https://github.com/wolfpld/tracy) for profiling the engine and more

VER 1.0 [BUG FIXES AND PERFORMANCE]

FIXES:

- Fixed detected crashes when loading saves and changing screen resolution
- Fixed crashes when opening sound settings (+ gamedata updated, replace it with the latest one from the archive)
- Fixed FPS drop in the PDA when tracking a quest with a target marker
- Fixed 'One Life' mode: all corresponding saves are now deleted after death
- Fixed and improved the engine error logger to version 2.1 (added a feature for the lazy: if the game crashes, the engine's ERROR folder containing the necessary files opens automatically)
- Other crash and bug fixes

OPTIMIZATION:

- NPC skeletons moved to multithreading and overhead reduced (according to tests, 90%+ of the heavy work is already processed on workers)
- Lua- and render-related optimizations
- Improved shadow cache for static objects (walls, lamps, etc.)
- Slightly reduced freeze when taking quests from NPCs
- Several other minor optimizations

ADDED QUARK-SCHEDULER TO THE ENGINE
Especially useful for hybrid CPUs (Intel P & E cores and multi-CCD Ryzen), given that this is a THREAD-BOUND engine.
orchestration_threads on is enabled by default, but you can disable it by typing orchestration_threads off in the console (it's enough to enter it once, and it stays permanent).

How it works:

>> Regular CPUs (Intel & AMD, SINGLE-CCD & SMT): the engine reads core rankings (CPPC on AMD / Turbo Boost Max 3.0 on Intel) and assigns the main thread an ideal processor (a hint to the Windows scheduler) on the first logical processor of the best core. The engine's other threads (workers, driver, sound) are not allowed onto this core, so there is no SMT-set splitting by engine threads and no local cache pollution. Once per second, outside the frame (meaning it doesn't affect the pipeline), the engine checks where the Windows scheduler is actually keeping the main thread and moves the reserve there. If core 'X' stays loaded for 2 seconds in a row (for example, by heavy threads from other software), core 'X' is excluded for 30 seconds and the main thread moves to another core. If CPPC is disabled on AMD and Intel has no Turbo Boost Max 3.0 (so the rankings cannot be read), the engine does not interfere with thread placement.
>> Intel with P/E cores (Alder Lake and newer): the engine distinguishes P-cores from E-cores by core classes. The main thread is allowed to run only on P-cores, and it also gets an ideal processor on the first logical processor of the best P-core. The engine's other threads (workers, driver, sound) run on all remaining cores, both P and E, but do not start on the core where the main thread is running, so the main thread's HT sibling is not occupied by engine threads (+ a cache boost, as I mentioned earlier). Once per second, outside the frame (doesn't affect the pipeline), the engine checks which P-core the Windows scheduler is actually keeping the main thread on and moves the reserve there. If P-core 'X' stays loaded for 2 seconds in a row (for example, by heavy threads from other software), core 'X' is excluded for 30 seconds and the main thread moves to another P-core. If all P-cores are busy, the main thread is temporarily allowed to run on E-cores.
>> AMD Dual-CCD (7900X/7950X/9950X and X3D): the engine picks the one CCD with the largest L3 (on X3D, the V-Cache CCD), and if L3 is equal, the one containing the core with the best CPPC ranking. All engine threads (main thread, workers, driver, sound) run only on this CCD, so there is no inter-CCD latency between the main thread and workers. Within the CCD the logic is the same as on regular CPUs (ideal processor for the main thread on the CCD's best core), and the engine's other threads are not allowed onto that core. If CPPC is disabled, only the CCD affinity remains.
+ On any CPU, the main thread is marked High QoS, which disables its throttling by the Windows scheduler.

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

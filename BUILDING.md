# Building Quark Engine

**English** · [Русский](BUILDING.ru.md)

## Tools

- Windows x64.
- Visual Studio 2022 with the **Desktop development with C++** workload.
- **MSVC v143**, x64/x86 build tools; the projects use this toolset.
- Windows 10/11 SDK.
- **C++ ATL** and **C++ MFC** components for v143.
- Git to obtain and update the source code.

This source package was verified by building `DX11-AVX | x64` with MSVC v143 **14.30.30705** and Windows SDK **10.0.26100.0**. Verification used MSBuild from Visual Studio 2026 Insiders with the v143 toolset installed; changing the projects to a different toolset was not required.

Dependencies are included in this source tree; an additional `git submodule update` is not required. LuaJIT, OpenAL Soft, LuaBind, ODE and codecs with included projects are built from source. Required prebuilt link libraries are located in `sdk/libraries/x64` and `src/3rd party/stackwalker/lib`.

## Getting the source code

```powershell
git clone https://github.com/quark-engine-stalker/quark-engine.git
cd quark-engine
```

## Building with Visual Studio

1. Open **`src/QUARK ENGINE.sln`**.
2. Select the **`DX11-AVX`** configuration and **`x64`** platform.
3. Build the entire solution: **Build → Build Solution**.

The internal projects use `ReleaseR4-AVX` and `Release-AVX`; the solution already provides the required configuration mappings. The configuration name is retained for compatibility, but the main build uses **AVX2**.

## Building with MSBuild

Open **Developer PowerShell for VS 2022** at the repository root:

```powershell
msbuild "src\QUARK ENGINE.sln" /t:Build /p:Configuration=DX11-AVX /p:Platform=x64 /m:4 /p:CL_MPCount=4
```

Adjust the number of parallel processes to fit the available memory. Use the same command for subsequent builds; for a full rebuild, replace `/t:Build` with `/t:Rebuild`.

Building individual `.vcxproj` files outside the solution context may fail to locate `Common.props`: relative paths are defined through `$(SolutionDir)`.

## Build output

The main output files are:

```text
_build/_game/bin_dbg/AnomalyDX11AVX.exe
_build/_game/bin_dbg/AnomalyDX11AVX.pdb
```

Other projects also create intermediate `.lib` and `.obj` files in `_build`. These files are excluded from Git.

To use the build in the game, replace the corresponding files in **`Anomaly\bin`** as shown in the [README](README.md#installation-and-requirements). If the `.pdb` is used for diagnostics, it must match that exact `.exe`.

## Building with the Tracy profiler

A separate build for profiling the engine and Lua scripts with [Tracy](https://github.com/wolfpld/tracy) 0.14.1. The Tracy client is located in `src/3rd party/tracy` and is compiled only into this build; the regular `.exe` does not contain it. It can be built only with MSBuild:

```powershell
msbuild "src\QUARK ENGINE.sln" /t:Build /p:Configuration=DX11-AVX /p:Platform=x64 /p:QuarkTracy=true /m:4 /p:CL_MPCount=4
```

Output:

```text
_build/_game_tracy/bin_dbg/AnomalyDX11AVX.exe
_build/_game_tracy/bin_dbg/AnomalyDX11AVX.pdb
```

Intermediate files go to separate folders with the `_tracy` suffix, so the regular build is not rebuilt afterwards.

Recording a profile:

1. Download Tracy 0.14.1 for Windows from the [release page](https://github.com/wolfpld/tracy/releases/tag/v0.14.1) and run `tracy-profiler.exe`. The profiler version must match the client version.
2. Replace the `.exe` and `.pdb` in `Anomaly\bin` with the Tracy build and start the game.
3. In the Tracy window, select `AnomalyDX11AVX` and click **Connect**. The engine collects no data until a profiler connects. Only connections from the same computer are accepted.

Additionally:

- When the game (MO2) runs as administrator, Tracy also collects CPU samples and context switches.
- The console command `tracy_alloc 1` sends every memory allocation with its call stack to Tracy. This slows the game down noticeably.
- To see individual Lua functions in the profile, mark a mod's scripts with `src/tools/tracy/prof_event_marker_gui.py`. It inserts `PROF_EVENT_BEGIN`/`PROF_EVENT_END` and keeps a `.bak` next to every file for restoring. Marked scripts also work in the regular build, but slower, so remove the markers after profiling.

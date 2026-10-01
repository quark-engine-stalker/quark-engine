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
git clone https://github.com/quark-engine-stalker/quark-engine-stalker.git
cd quark-engine-stalker
```

Your GitHub account needs read access to clone a private repository.

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

To use the build in the game, replace the corresponding files in **`Anomaly\bin`** as shown in the [README](README.md#installation-and-system-requirements). If the `.pdb` is used for diagnostics, it must match that exact `.exe`.

<p align="center">
  <img src="docs/assets/quark-banner.png" alt="QUARK ENGINE - X-Ray Monolith fork" width="100%">
</p>

<p align="center">
  <img src="docs/assets/perf.png" alt="Performance" width="100%">
</p>

<p align="center">
  <b>English</b> & <a href="README.ru.md">Русский</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-OPEN_BETA-c2df77?style=flat-square&amp;labelColor=171d20" alt="Open beta">
  <img src="https://img.shields.io/badge/platform-Windows_x64-8ca6b8?style=flat-square&amp;labelColor=171d20" alt="Windows x64">
  <img src="https://img.shields.io/badge/renderer-DX11_%2F_AVX2-8ca6b8?style=flat-square&amp;labelColor=171d20" alt="DirectX 11 / AVX2">
</p>

<p align="center">
  <b>The project's goal is to optimize the use of modern hardware to improve performance and resolve numerous technical issues while maintaining compatibility with mods and modpacks.</b>
</p>

<p align="center">
  <a href="https://github.com/quark-engine-stalker/quark-engine/releases">Download / Releases</a> &
  <a href="#installation-and-requirements">Installation</a> &
  <a href="CHANGELOG.md">Changelog</a> &
  <a href="#documentation">Documentation</a>
</p>

---

## Support and Changelog

The engine is tested with **STALKER: Anomaly 1.5.3** and **STALKER: GAMMA 0.9.5**.  
Update history: [CHANGELOG.md](CHANGELOG.md).

## Installation and Requirements

Your processor must support **AVX2**. Also install & update the [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe) libraries.

1. Make sure STALKER: Anomaly 1.5.3 and STALKER: GAMMA 0.9.5 are installed correctly. Installation instructions are available from their respective authors.
2. **Make a backup** of the `Anomaly\bin` and `Anomaly\gamedata` folders.
3. Download the latest `AnomalyDX11AVX.rar` archive from the [releases page](https://github.com/quark-engine-stalker/quark-engine/releases) and extract it.
4. Copy `AnomalyDX11AVX.exe` and `AnomalyDX11AVX.pdb` to the `Anomaly\bin` folder, replacing the existing files.
5. Copy the `gamedata` folder from the archive to the root of the `Anomaly` folder (next to `bin`), replacing the existing files.
6. Launch **Anomaly Launcher** through MO2 (Mod Organizer), enable **Support for AVX CPU** and **Delete Shader Cache**, and start the game. Shaders will be recompiled on the first launch.

It is recommended to leave the graphics settings at their defaults. If you encounter errors, attach the logs from the `AppData\Roaming\QUARK-ENGINE\ERROR` folder to your problem report and send everything to us on Discord.

<p align="center">
  <img src="docs/assets/install-replace.png" alt="Replacing files" width="655">
  <img src="docs/assets/Launcher.png" alt="Launching through the launcher" width="655">
  <img src="docs/assets/gamedata.png" alt="Replacing gamedata" width="655">
</p>

## Source Code and Building

```text
quark-engine/
├── src/                 # Main engine source code
│   ├── 3rd party/       # Third-party libraries and their source code
│   └── QUARK ENGINE.sln
├── sdk/                 # Headers, codec sources, and libraries used for linking
├── docs/assets/         # README assets and installation illustrations
├── licenses/            # License texts for dependencies
├── CHANGELOG.md         # Version history in English
├── CHANGELOG.ru.md      # Version history in Russian
└── License.txt          # X-Ray distribution terms
```

The primary build configuration is **`DX11-AVX | x64`**. For details and the MSBuild command, see the [build guide](BUILDING.md).

## Documentation

| Document | English | Русский |
| :--- | :--- | :--- |
| Project overview and installation | [README](README.md) | [README](README.ru.md) |
| Building from source | [Building](BUILDING.md) | [Сборка](BUILDING.ru.md) |
| Version history | [Changelog](CHANGELOG.md) | [История изменений](CHANGELOG.ru.md) |

## Project Authors

QUARK-ENGINE is based on [X-Ray Monolith](https://github.com/themrdemonized/xray-monolith).

## License

The original X-Ray Engine used by S.T.A.L.K.E.R. belongs to **GSC Game World**. Distribution terms: [License.txt](License.txt).

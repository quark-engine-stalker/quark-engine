<p align="center">
  <img src="docs/assets/quark-banner.png" alt="QUARK ENGINE — X-Ray Monolith fork" width="100%">
</p>

<p align="center">
  <b>English</b> · <a href="README.ru.md">Русский</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-OPEN_BETA-c2df77?style=flat-square&amp;labelColor=171d20" alt="Open beta">
  <img src="https://img.shields.io/badge/platform-Windows_x64-8ca6b8?style=flat-square&amp;labelColor=171d20" alt="Windows x64">
  <img src="https://img.shields.io/badge/renderer-DX11_%2F_AVX2-8ca6b8?style=flat-square&amp;labelColor=171d20" alt="DirectX 11 / AVX2">
  <a href="https://discord.com/invite/X7GRYsNNEc"><img src="https://img.shields.io/badge/Discord-community-5865F2?style=flat-square&amp;logo=discord&amp;logoColor=white" alt="Join Discord"></a>
</p>

<p align="center">
  <b>Quark Engine is a fork of X-Ray Monolith. Its goal is to make better use of modern hardware, improve performance and resolve technical issues while preserving compatibility with mods and modpacks.</b>
</p>

<p align="center">
  <a href="https://github.com/quark-engine-stalker/quark-engine-stalker/releases">Downloads / Releases</a> ·
  <a href="#installation-and-system-requirements">Installation</a> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="#documentation">Documentation</a> ·
  <a href="https://discord.com/invite/X7GRYsNNEc">Discord</a>
</p>

---

## Support and updates

Engine builds and debug symbols are available through **[GitHub Releases](https://github.com/quark-engine-stalker/quark-engine-stalker/releases)**. Select a release and download its `.exe` and `.pdb` from **Assets**. These files appear in Assets once the author uploads a build.

The engine is being tested on **STALKER: Anomaly 1.5.3** and **STALKER: GAMMA 0.9.5**.
See [CHANGELOG.md](CHANGELOG.md) for the update history.

## Installation and system requirements

**System requirements:** a processor with **AVX2** support and the [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe).

1. Make sure **STALKER: Anomaly 1.5.3** and **STALKER: GAMMA 0.9.5** are installed correctly. Refer to their authors' installation instructions.
2. Back up the engine files you are replacing in **`Anomaly\bin`**.
3. Copy `AnomalyDX11AVX.exe` and its matching `AnomalyDX11AVX.pdb` to **`Anomaly\bin`** and choose **Replace the files in the destination**.
4. Launch the game through **MO2 (Mod Organizer)**. Installation and launch examples are shown below.
5. Compatibility with existing saves is not guaranteed; starting a new game is recommended.
6. If you encounter errors or crashes, send us the logs from **`AppData\Roaming\QUARK ENGINE\ERROR`** through Discord. These files help identify the cause of the failure.

<p align="center">
  <img src="docs/assets/install-replace.png" alt="Installation: replace the engine files in Anomaly\bin" width="655">
  <img src="docs/assets/install-mo2.png" alt="Launch the game through MO2 (Mod Organizer)" width="655">
</p>

## Source code and building

```text
quark-engine-stalker/
├── src/                 # Engine source code
│   ├── 3rd party/       # Third-party libraries and their source code
│   └── QUARK ENGINE.sln
├── sdk/                 # Headers, codec sources and link libraries
├── docs/assets/         # README artwork and installation screenshots
├── licenses/            # Dependency license texts
├── CHANGELOG.md         # Version history in English
├── CHANGELOG.ru.md      # Version history in Russian
└── License.txt          # X-Ray distribution terms
```

The main build configuration is **`DX11-AVX | x64`**. See the [build guide](BUILDING.md) for details and the MSBuild command.

## Documentation

Each document is available in English and Russian. Use the language selector at the top of a page to switch languages.

| Document | English | Русский |
| :--- | :--- | :--- |
| Project overview and installation | [README](README.md) | [README](README.ru.md) |
| Building from source | [Building](BUILDING.md) | [Сборка](BUILDING.ru.md) |
| Version history | [Changelog](CHANGELOG.md) | [История изменений](CHANGELOG.ru.md) |
| Code provenance and third-party components | [Third-party components](THIRD_PARTY.md) | [Сторонние компоненты](THIRD_PARTY.ru.md) |
| Release description template | [Release template](docs/release-template.md) | [Шаблон релиза](docs/release-template.ru.md) |

## Community and project authors

Join **[DISCORD QUARK ENGINE](https://discord.com/invite/X7GRYsNNEc)**.

Quark Engine is based on [X-Ray Monolith](https://github.com/themrdemonized/xray-monolith). See [THIRD_PARTY.md](THIRD_PARTY.md) for code provenance and third-party notices.

## License

The original S.T.A.L.K.E.R. X-Ray Engine belongs to **GSC Game World**. Distribution terms: [License.txt](License.txt).

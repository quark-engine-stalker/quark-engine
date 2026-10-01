# Code provenance and third-party components

**English** · [Русский](THIRD_PARTY.ru.md)

Quark Engine is based on [X-Ray Monolith](https://github.com/themrdemonized/xray-monolith), which builds on X-Ray 1.6 / Call of Chernobyl / Anomaly. Modifications do not override the original copyright notices or distribution terms.

## Engine code

[License.txt](License.txt) retains the license text from X-Ray Monolith. It permits distribution of source and binary code with the required notices retained, and prohibits commercial use. The original engine belongs to **GSC Game World**.

Source of the restored text: [License.txt in upstream Monolith](https://github.com/themrdemonized/xray-monolith/blob/9b491ffbfe5d529e817ceffed70214d0f03f62c5/License.txt). This commit documents the provenance of the license text; it does not identify the version from which all Quark Engine changes were ported.

## Libraries

The terms below apply to each corresponding component, not to the entire project. Original notices in the source files are retained.

| Component | Location | Terms and source |
| :--- | :--- | :--- |
| LuaJIT, DynASM | `src/3rd party/luajit-2` | [COPYRIGHT](src/3rd%20party/luajit-2/COPYRIGHT); [exact snapshot version](src/3rd%20party/luajit-2/UPSTREAM.txt) |
| LuaBind | `src/3rd party/luabind` | MIT; [retained notice](licenses/luabind/LICENSE) |
| OpenAL Soft 1.23.1 | `src/3rd party/OpenAL-new` | LGPL 2 / 2.1 or later, according to the source file notices; [COPYING](licenses/openal-soft/COPYING), [upstream](https://github.com/kcat/openal-soft/tree/1.23.1) |
| Dear ImGui 1.91.8 | `src/3rd party/imgui` | MIT; [LICENSE](licenses/imgui/LICENSE.txt), [upstream](https://github.com/ocornut/imgui/tree/v1.91.8) |
| ODE | `src/3rd party/ode` | BSD or LGPL, according to the headers; [BSD text](licenses/ode/LICENSE-BSD.TXT), [LGPL 2.1](licenses/ode/LICENSE.TXT) |
| OpenSSL 0.9.8j | `src/3rd party/crypto/openssl` | OpenSSL / SSLeay; [LICENSE](licenses/openssl/LICENSE) |
| CxImage | `src/3rd party/cximage` | [license.txt](src/3rd%20party/cximage/license.txt) |
| giflib | `src/3rd party/giflib` | MIT; [COPYING](licenses/giflib/COPYING) |
| NVIDIA Texture Tools | `src/3rd party/NVTT` | [NVTT notice](licenses/nvtt/LICENSE); additional terms for individual files, including Squish, are retained in the sources |
| robin_hood hashing | `src/3rd party/robin_hood` | MIT; [LICENSE](licenses/robin-hood/LICENSE) |
| StackWalker | `src/3rd party/stackwalker` | BSD; [retained notice](licenses/stackwalker/LICENSE) |
| Intel TBB 2020 | `src/3rd party/tbb`, `sdk/libraries/x64/tbb.lib` | Apache 2.0; [LICENSE](licenses/tbb/LICENSE) |
| ICU | `src/3rd party/icu`, `sdk/libraries/x64/icuuc.lib` | Unicode / ICU; [ICU 65 license and third-party notices](licenses/icu/LICENSE) |
| Boost headers | `sdk/include/sm_boost` | Boost Software License 1.0; [LICENSE](licenses/boost/LICENSE_1_0.txt) |
| libogg | `sdk/include/ogg` | BSD; [COPYING](licenses/ogg/COPYING) |
| libvorbis | `sdk/include/vorbis` | BSD; [COPYING](licenses/vorbis/COPYING) |
| libtheora | `sdk/include/theora` | BSD; [COPYING](licenses/theora/COPYING) |
| Independent JPEG Group, release 6b | `sdk/include/jpeg` | [README, LEGAL ISSUES section](sdk/include/jpeg/README) |
| ReShade API | `src/3rd party/reshade`, `src/ReShadeCompat` | Notices and terms in the corresponding API headers; [upstream](https://github.com/crosire/reshade) |
| FastDelegate, fast_dynamic_cast and other included components | `src/3rd party` | Copyright notices and terms in the corresponding source files |

For small libraries, notice text already embedded in the sources has also been copied to `licenses/` without changing its terms. Including a third-party component in the tree does not make its license apply to the entire engine.

## SDK and link libraries

Parts of the compatible SDK are inherited from the Monolith source tree: Microsoft DirectX headers / D3DX import libraries, NVIDIA NVAPI and the Discord Game SDK. The respective vendors' terms apply, including notices in the headers. The X-Ray license does not grant additional rights to these components.

Only third-party libraries required for the current build are retained in `sdk/libraries/x64`. Standard Windows import libraries are supplied by the Windows SDK. The presence of `d3dx9.lib` is related to inherited math and resource helper functions; it does not add a DX9 renderer to the available Quark Engine configurations.

Runtime DLLs, game assets and prebuilt executables are not included in the source repository. Original rights to S.T.A.L.K.E.R. and its game data remain with their respective rights holders.

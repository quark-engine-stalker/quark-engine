# Происхождение кода и сторонние компоненты

[English](THIRD_PARTY.md) · **Русский**

Quark Engine основан на [X-Ray Monolith](https://github.com/themrdemonized/xray-monolith), который развивает X-Ray 1.6 / Call of Chernobyl / Anomaly. Внесённые изменения не отменяют исходные уведомления об авторских правах и условия распространения.

## Код движка

[License.txt](License.txt) сохраняет текст лицензии из X-Ray Monolith. Она допускает распространение исходного и бинарного кода с сохранением уведомлений и запрещает коммерческое использование. Оригинальный движок принадлежит **GSC Game World**.

Источник восстановленного текста: [License.txt в upstream Monolith](https://github.com/themrdemonized/xray-monolith/blob/9b491ffbfe5d529e817ceffed70214d0f03f62c5/License.txt). Этот commit указан для происхождения документа; он не обозначает версию, из которой были перенесены все изменения Quark Engine.

## Библиотеки

Условия ниже относятся к соответствующему компоненту, а не к проекту целиком. Исходные уведомления в файлах сохранены.

| Компонент | Расположение | Условия и источник |
| :--- | :--- | :--- |
| LuaJIT, DynASM | `src/3rd party/luajit-2` | [COPYRIGHT](src/3rd%20party/luajit-2/COPYRIGHT); [точная версия snapshot](src/3rd%20party/luajit-2/UPSTREAM.txt) |
| LuaBind | `src/3rd party/luabind` | MIT; [сохранённое уведомление](licenses/luabind/LICENSE) |
| OpenAL Soft 1.23.1 | `src/3rd party/OpenAL-new` | LGPL 2 / 2.1 или более поздняя версия согласно уведомлениям файлов; [COPYING](licenses/openal-soft/COPYING), [upstream](https://github.com/kcat/openal-soft/tree/1.23.1) |
| Dear ImGui 1.91.8 | `src/3rd party/imgui` | MIT; [LICENSE](licenses/imgui/LICENSE.txt), [upstream](https://github.com/ocornut/imgui/tree/v1.91.8) |
| ODE | `src/3rd party/ode` | BSD или LGPL согласно заголовкам; [BSD-текст](licenses/ode/LICENSE-BSD.TXT), [LGPL 2.1](licenses/ode/LICENSE.TXT) |
| OpenSSL 0.9.8j | `src/3rd party/crypto/openssl` | OpenSSL / SSLeay; [LICENSE](licenses/openssl/LICENSE) |
| CxImage | `src/3rd party/cximage` | [license.txt](src/3rd%20party/cximage/license.txt) |
| giflib | `src/3rd party/giflib` | MIT; [COPYING](licenses/giflib/COPYING) |
| NVIDIA Texture Tools | `src/3rd party/NVTT` | [Уведомление NVTT](licenses/nvtt/LICENSE); дополнительные условия отдельных файлов, включая Squish, сохранены в исходниках |
| robin_hood hashing | `src/3rd party/robin_hood` | MIT; [LICENSE](licenses/robin-hood/LICENSE) |
| StackWalker | `src/3rd party/stackwalker` | BSD; [сохранённое уведомление](licenses/stackwalker/LICENSE) |
| Intel TBB 2020 | `src/3rd party/tbb`, `sdk/libraries/x64/tbb.lib` | Apache 2.0; [LICENSE](licenses/tbb/LICENSE) |
| ICU | `src/3rd party/icu`, `sdk/libraries/x64/icuuc.lib` | Unicode / ICU; [LICENSE и сторонние уведомления ICU 65](licenses/icu/LICENSE) |
| Boost headers | `sdk/include/sm_boost` | Boost Software License 1.0; [LICENSE](licenses/boost/LICENSE_1_0.txt) |
| libogg | `sdk/include/ogg` | BSD; [COPYING](licenses/ogg/COPYING) |
| libvorbis | `sdk/include/vorbis` | BSD; [COPYING](licenses/vorbis/COPYING) |
| libtheora | `sdk/include/theora` | BSD; [COPYING](licenses/theora/COPYING) |
| Independent JPEG Group, release 6b | `sdk/include/jpeg` | [README, раздел LEGAL ISSUES](sdk/include/jpeg/README) |
| ReShade API | `src/3rd party/reshade`, `src/ReShadeCompat` | Уведомления и условия в соответствующих API-заголовках; [upstream](https://github.com/crosire/reshade) |
| FastDelegate, fast_dynamic_cast, прочие включённые компоненты | `src/3rd party` | Авторские уведомления и условия в соответствующих исходных файлах |

Для небольших библиотек текст, уже встроенный в исходники, дополнительно вынесен в `licenses/` без изменения условий. Сохранение стороннего компонента в дереве не означает, что его лицензия распространяется на весь движок.

## SDK и библиотеки для линковки

Часть совместимого SDK унаследована из дерева Monolith: Microsoft DirectX headers / D3DX import libraries, NVIDIA NVAPI и Discord Game SDK. К ним применяются условия соответствующих поставщиков, в том числе уведомления в заголовках. Лицензия X-Ray не предоставляет дополнительных прав на эти компоненты.

В `sdk/libraries/x64` оставлены только сторонние библиотеки, необходимые для текущей сборки. Стандартные Windows import libraries предоставляются Windows SDK. Наличие `d3dx9.lib` связано с унаследованными вспомогательными функциями математики и ресурсов; оно не добавляет DX9 renderer в доступные конфигурации Quark Engine.

Runtime DLL, игровые ресурсы и готовые исполняемые файлы не включены в исходный репозиторий. Оригинальные права на S.T.A.L.K.E.R. и игровые данные сохраняются за их правообладателями.

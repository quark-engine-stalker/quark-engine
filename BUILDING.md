# Сборка Quark Engine

## Инструменты

- Windows x64.
- Visual Studio 2022 с workload **Desktop development with C++**.
- **MSVC v143**, x64/x86 build tools; проекты используют этот toolset.
- Windows 10/11 SDK.
- Компоненты **C++ ATL** и **C++ MFC** для v143.
- Git — для получения и обновления исходников.

Подготовленный набор проверен сборкой `DX11-AVX | x64` на MSVC v143 **14.30.30705** и Windows SDK **10.0.26100.0**. При проверке использовался MSBuild из Visual Studio 2026 Insiders с установленным toolset v143; переключение проектов на другой toolset не требовалось.

В этой копии исходников библиотеки vendored: дополнительный `git submodule update` не требуется. LuaJIT, OpenAL Soft, LuaBind, ODE и кодеки, для которых включены проекты, собираются из исходников. Необходимые готовые библиотеки для линковки находятся в `sdk/libraries/x64` и `src/3rd party/stackwalker/lib`.

## Получение исходников

```powershell
git clone https://github.com/quark-engine-stalker/quark-engine-stalker.git
cd quark-engine-stalker
```

Для приватного репозитория GitHub-аккаунту нужен доступ на чтение.

## Через Visual Studio

1. Откройте **`src/QUARK ENGINE.sln`**.
2. Выберите **`DX11-AVX`** и платформу **`x64`**.
3. Соберите solution целиком: **Build → Build Solution**.

Внутренние проекты используют `ReleaseR4-AVX` и `Release-AVX`; нужное соответствие уже задано в solution. Имя конфигурации сохранено для совместимости, однако основная сборка использует **AVX2**.

## Через MSBuild

Откройте **Developer PowerShell for VS 2022** в корне репозитория:

```powershell
msbuild "src\QUARK ENGINE.sln" /t:Build /p:Configuration=DX11-AVX /p:Platform=x64 /m:4 /p:CL_MPCount=4
```

Число параллельных процессов можно изменить с учётом доступной памяти. Для повторной сборки используйте ту же команду; для полной пересборки замените `/t:Build` на `/t:Rebuild`.

Сборка отдельных `.vcxproj` без контекста solution может не найти `Common.props`: относительные пути определены через `$(SolutionDir)`.

## Результат

Основные выходные файлы:

```text
_build/_game/bin_dbg/AnomalyDX11AVX.exe
_build/_game/bin_dbg/AnomalyDX11AVX.pdb
```

Другие проекты также создают промежуточные `.lib` и `.obj` в `_build`. Эти файлы исключены из Git.

Для игры замените соответствующие файлы в **`Anomaly\bin`**, как показано в [README](README.md#установка). `.pdb` должен соответствовать именно этому `.exe`, если он используется для диагностики. Существующие runtime DLL Anomaly / Monolith / GAMMA должны оставаться доступны в игровой установке; SDK-библиотеки `.lib` предназначены для сборки и в `Anomaly\bin` не копируются.

## Особенности текущей копии

- История обновлений включает **0.0.3 Open Beta**. Встроенные строки версии в `x_ray.cpp` и `resource.rc` этой копии пока указывают **0.0.2**; это сохранённая метаинформация исходного рабочего дерева.
- Игровые скрипты, шейдеры и данные не входят в этот репозиторий. Используйте установленную Anomaly / GAMMA с подходящими данными Monolith.
- Лицензия X-Ray и отдельные лицензии зависимостей сохраняются: [License.txt](License.txt), [THIRD_PARTY.md](THIRD_PARTY.md).
- Успешная компиляция подтверждает полноту набора для сборки. Совместимость и производительность в игре проверяются отдельно на реальных сохранениях.

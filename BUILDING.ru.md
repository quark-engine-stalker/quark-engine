# Сборка Quark Engine

[English](BUILDING.md) · **Русский**

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
git clone https://github.com/quark-engine-stalker/quark-engine.git
cd quark-engine
```

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

Для игры замените соответствующие файлы в **`Anomaly\bin`**, как показано в [README](README.ru.md#установка-и-требования). `.pdb` должен соответствовать именно этому `.exe`, если он используется для диагностики.

## Сборка с профайлером Tracy

Отдельная сборка для профилирования движка и Lua-скриптов в [Tracy](https://github.com/wolfpld/tracy) 0.14.1. Клиент Tracy находится в `src/3rd party/tracy` и подключается только в этой сборке, в обычном `.exe` его нет. Собирается только через MSBuild:

```powershell
msbuild "src\QUARK ENGINE.sln" /t:Build /p:Configuration=DX11-AVX /p:Platform=x64 /p:QuarkTracy=true /m:4 /p:CL_MPCount=4
```

Результат:

```text
_build/_game_tracy/bin_dbg/AnomalyDX11AVX.exe
_build/_game_tracy/bin_dbg/AnomalyDX11AVX.pdb
```

Промежуточные файлы пишутся в отдельные папки с суффиксом `_tracy`, поэтому обычная сборка после этого не пересобирается.

Запись профиля:

1. Скачайте Tracy 0.14.1 для Windows со [страницы релиза](https://github.com/wolfpld/tracy/releases/tag/v0.14.1) и запустите `tracy-profiler.exe`. Версия профайлера должна совпадать с версией клиента.
2. Замените `.exe` и `.pdb` в `Anomaly\bin` файлами Tracy-сборки и запустите игру.
3. В окне Tracy выберите `AnomalyDX11AVX` и нажмите **Connect**. Пока профайлер не подключён, движок данные не собирает. Подключение принимается только с этого же компьютера.

Дополнительно:

- Если запустить игру (MO2) от имени администратора, Tracy также собирает сэмплы CPU и переключения контекста.
- Консольная команда `tracy_alloc 1` передаёт в Tracy каждое выделение памяти со стеком вызовов. Игра при этом заметно замедляется.
- Чтобы видеть в профиле отдельные Lua-функции, разметьте скрипты мода утилитой `src/tools/tracy/prof_event_marker_gui.py`. Она расставляет `PROF_EVENT_BEGIN`/`PROF_EVENT_END` и сохраняет рядом с каждым файлом `.bak` для отката. Размеченные скрипты работают и в обычной сборке, но медленнее, поэтому после профилирования разметку нужно снять.

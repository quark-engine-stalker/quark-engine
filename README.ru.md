<p align="center">
  <img src="docs/assets/quark-banner.png" alt="QUARK ENGINE - X-Ray Monolith fork" width="100%">
</p>

<p align="center">
  <img src="docs/assets/perf.png" alt="Performance" width="100%">
</p>

<p align="center">
  <a href="README.md">English</a> & <b>Русский</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-OPEN_BETA-c2df77?style=flat-square&amp;labelColor=171d20" alt="Open beta">
  <img src="https://img.shields.io/badge/platform-Windows_x64-8ca6b8?style=flat-square&amp;labelColor=171d20" alt="Windows x64">
  <img src="https://img.shields.io/badge/renderer-DX11_%2F_AVX2-8ca6b8?style=flat-square&amp;labelColor=171d20" alt="DirectX 11 / AVX2">
</p>

<p align="center">
  <b>Цель проекта заключается в оптимизации использования современного железа для повышения производительности и устранении множества технических проблем, сохраняя при этом совместимость с модами и сборками.</b>
</p>

<p align="center">
  <a href="https://github.com/quark-engine-stalker/quark-engine/releases">Скачать / Releases</a> &
  <a href="#установка-и-требования">Установка</a> &
  <a href="CHANGELOG.ru.md">История изменений</a> &
  <a href="#документация">Документация</a>
</p>

---

## Поддержка и список изменений

Движок тестируется на **STALKER: Anomaly 1.5.3** и **STALKER: GAMMA 0.9.5**.
История обновлений: [CHANGELOG.ru.md](CHANGELOG.ru.md).

## Установка и требования

Ваш процессор должен быть с поддержкой **AVX2**, также установите & обновите библиотеки [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe).

1. Убедитесь, что STALKER: Anomaly 1.5.3 и STALKER: GAMMA 0.9.5 установлены корректно. Инструкции по их установке есть у соответствующих авторов.
2. **Сделайте резервную копию** папки `Anomaly\bin` и папки `Anomaly\gamedata`.
3. Скачайте последний архив `AnomalyDX11AVX.rar` со [страницы релизов](https://github.com/quark-engine-stalker/quark-engine/releases) и распакуйте его.
4. Скопируйте `AnomalyDX11AVX.exe` и `AnomalyDX11AVX.pdb` в папку `Anomaly\bin` с заменой существующих файлов.
5. Скопируйте папку `gamedata` из архива в корень папки `Anomaly` (рядом с `bin`) с заменой существующих файлов.
6. Запустите через MO2 (Mod Organizer) **Anomaly Launcher**, включите **Support for AVX CPU** и **Delete Shader Cache** и запустите игру. Шейдеры перекомпилируются при первом запуске.

Настройки графики рекомендуется оставить по умолчанию. При ошибках приложите к сообщению о проблеме логи из папки `AppData\Roaming\QUARK-ENGINE\ERROR` и отправьте все это нам в Discord.


<p align="center">
  <img src="docs/assets/install-replace.png" alt="замена файлов" width="655">
  <img src="docs/assets/Launcher.png" alt="запуск через лаунчер" width="655">
  <img src="docs/assets/gamedata.png" alt="замена gamedata" width="655">
</p>

## Исходники и сборка

```text
quark-engine/
├── src/                 # Основной код движка
│   ├── 3rd party/       # Сторонние библиотеки и их исходники
│   └── QUARK ENGINE.sln
├── sdk/                 # Заголовки, исходники кодеков, библиотеки для линковки
├── docs/assets/         # Оформление README и иллюстрация установки
├── licenses/            # Лицензионные тексты зависимостей
├── CHANGELOG.md         # История версий на английском
├── CHANGELOG.ru.md      # История версий на русском
└── License.txt          # Условия распространения X-Ray
```

Основная конфигурация: **`DX11-AVX | x64`**. Подробности и команда MSBuild в [руководстве по сборке](BUILDING.ru.md).

## Документация

| Документ | English | Русский |
| :--- | :--- | :--- |
| Обзор проекта и установка | [README](README.md) | [README](README.ru.md) |
| Сборка из исходников | [Building](BUILDING.md) | [Сборка](BUILDING.ru.md) |
| История версий | [Changelog](CHANGELOG.md) | [История изменений](CHANGELOG.ru.md) |

## Авторы проекта

QUARK-ENGINE основан на [X-Ray Monolith](https://github.com/themrdemonized/xray-monolith).

## Лицензия

Оригинальный X-Ray Engine игры S.T.A.L.K.E.R. принадлежит **GSC Game World**. Условия распространения: [License.txt](License.txt).

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
  <a href="https://discord.com/invite/X7GRYsNNEc"><img src="https://img.shields.io/badge/Discord-сообщество-5865F2?style=flat-square&amp;logo=discord&amp;logoColor=white" alt="Перейти в Discord"></a>
</p>

<p align="center">
  <b>Цель проекта заключается в оптимизации использования современного железа для повышения производительности и устранении множества технических проблем, сохраняя при этом совместимость с модами и сборками.</b>
</p>

<p align="center">
  <a href="https://github.com/quark-engine-stalker/quark-engine/releases">Скачать / Releases</a> &
  <a href="#установка-движка-и-требования">Установка</a> &
  <a href="CHANGELOG.ru.md">История изменений</a> &
  <a href="#документация">Документация</a> &
  <a href="https://discord.com/invite/X7GRYsNNEc">Discord</a>
</p>

---

## Поддержка и список изменений

Движок тестируется на **STALKER: Anomaly 1.5.3** и **STALKER: GAMMA 0.9.5**.
История обновлений: [CHANGELOG.ru.md](CHANGELOG.ru.md).

## Установка и требования

Ваш процессор должен быть с поддержкой **AVX2**, также установите & обновите библиотеки [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe).

1. Убедитесь, что STALKER: Anomaly 1.5.3 и STALKER: GAMMA 0.9.5 установлены корректно. Инструкции по установке ищите у соответствующих авторов.
2. Скачайте последнюю версию `AnomalyDX11AVX.rar` из нашего репозитория на GitHub. Извлеките файлы `.exe` и `.pdb` в папку `Anomaly\bin`, подтвердив замену существующих файлов. **Перед заменой обязательно сделайте резервную копию на всякий случай.**
3. Запустите игру через MO2 (Mod Organizer). Выберите и запустите **Anomaly Launcher**, включите параметры **Support for AVX CPU** и **Delete Shader Cache**, а затем запустите игру. Это необходимо для перекомпиляции шейдеров.
4. Если игра вылетает при загрузке сохранения, скопируйте папку `gamedata` из последнего архива на GitHub в корневую директорию Anomaly, подтвердив замену существующих файлов. После этого начните новую игру. **Перед заменой обязательно сделайте резервную копию на всякий случай.**
5. Рекомендуется оставить настройки графики по умолчанию.
6. Если возникают ошибки, отправьте нам в Discord логи из папки `AppData\Roaming\QUARK ENGINE\ERROR`. Эти файлы помогают определить причины вылетов и в дальнейшем улучшать движок.


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
| Шаблон описания релиза | [Release template](docs/release-template.md) | [Шаблон релиза](docs/release-template.ru.md) |

## Сообщество и авторы проекта

Присоединяйтесь к нам: **[DISCORD QUARK ENGINE](https://discord.com/invite/X7GRYsNNEc)**.
Quark Engine основан на [X-Ray Monolith](https://github.com/themrdemonized/xray-monolith).

## Лицензия

Оригинальный X-Ray Engine игры S.T.A.L.K.E.R. принадлежит **GSC Game World**. Условия распространения: [License.txt](License.txt).

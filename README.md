<p align="center">
  <img src="docs/assets/quark-banner.png" alt="QUARK-ENGINE - X-Ray Monolith fork" width="100%">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-OPEN_BETA-c2df77?style=flat-square&amp;labelColor=171d20" alt="Open beta">
  <img src="https://img.shields.io/badge/platform-Windows_x64-8ca6b8?style=flat-square&amp;labelColor=171d20" alt="Windows x64">
  <img src="https://img.shields.io/badge/renderer-DX11_%2F_AVX2-8ca6b8?style=flat-square&amp;labelColor=171d20" alt="DirectX 11 / AVX2">
  <a href="https://discord.com/invite/X7GRYsNNEc"><img src="https://img.shields.io/badge/Discord-сообщество-5865F2?style=flat-square&amp;logo=discord&amp;logoColor=white" alt="Перейти в Discord"></a>
</p>

<p align="center">
  <b>Цель движка заключается в оптимизации использования современного железа для повышения производительности и устранении множества технических проблем, сохраняя при этом совместимость с модами и сборками.</b><br>
</p>

<p align="center">
  <a href="https://github.com/quark-engine-stalker/quark-engine-stalker/releases">Скачать / Releases</a> ·
  <a href="#установка">Установка</a> ·
  <a href="CHANGELOG.md">История изменений</a> ·
  <a href="https://discord.com/invite/X7GRYsNNEc">Discord</a>
</p>

---

## Поддержка и список изменений

Готовые сборки движка и файлы отладки: **[GitHub Releases](https://github.com/quark-engine-stalker/quark-engine-stalker/releases)**. Выберите нужную версию и скачайте `.exe` и `.pdb` из раздела **Assets**. Файлы появляются в Assets после загрузки автором сборки.

Порядок загрузки файлов и выпуска следующих версий: [RELEASING.md](RELEASING.md).

Движок тестируется на **STALKER: Anomaly 1.5.3** и **STALKER: GAMMA 0.9.5**.
История обновлений: [CHANGELOG.md](CHANGELOG.md).

## Установка движка и технические требования

**Технические требования:** Процессор с поддержкой **AVX2** инструкций, установленные библиотеки [Microsoft Visual C++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe).

0. Наличие корректно установленого STALKER: GAMMA 0.9.5 & STALKER: Anomaly 1.5.3 (Инструкции по установке ищите у их авторов)
1. Сохраните резервную копию заменяемых файлов из **`Anomaly\bin`**. (на всякий случай)
3. Скопируйте файлы AnomalyDX11AVX.exe и AnomalyDX11AVX.pdb в папку **`Anomaly\bin`** и выберите **'Заменить в папке назначения'**.
4. Запуск игры осуществляется через MO2 (ModOrganizer), скриншоты как должно быть представлены ниже.
5. Совместимость с текущими сохранениями не гарантируется, рекомендуется начать новую игру.
6. При возникновении багов & крашей и тд, по пути: **`AppData\Roaming\QUARK ENGINE\ERROR`** находятся файлы логов о сбоях, отправляйте их нам и тем самым вы помогаете улучшать движок.
<p align="center">
  <img src="docs/assets/install-replace.png" alt="Установка: замена AnomalyDX11AVX.exe в папке Anomaly\bin" width="655">
  <img src="docs/assets/install-mo2.png" alt="Запуск: через лаунчер MO2 (ModOrganizer)" width="655">
</p>

## Исходники и сборка

```text
quark-engine-stalker/
├── src/                 # Основной код движка
│   ├── 3rd party/       # Сторонние библиотеки и их исходники
│   └── QUARK ENGINE.sln
├── sdk/                 # Заголовки, исходники кодеков, библиотеки для линковки
├── docs/assets/         # Оформление README и иллюстрация установки
├── licenses/            # Лицензионные тексты зависимостей
├── CHANGELOG.md         # История версий
└── License.txt          # Условия распространения X-Ray
```

Основная конфигурация: **`DX11-AVX | x64`**. Подробности и команда MSBuild в [BUILDING.md](BUILDING.md).

## Сообщество и авторы проекта

Переходите в наш: **[DISCORD QUARK-ENGINE](https://discord.com/invite/X7GRYsNNEc)**.

## License

**GSC Game World**: оригинальный X-Ray Engine игры S.T.A.L.K.E.R. [License.txt](License.txt)

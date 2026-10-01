# Выпуск новых версий Quark Engine

Готовые сборки и файлы отладки публикуются в **[GitHub Releases](https://github.com/quark-engine-stalker/quark-engine-stalker/releases)**.

## Добавить EXE и PDB к версии 0.0.3

1. Откройте [релиз 0.0.3](https://github.com/quark-engine-stalker/quark-engine-stalker/releases/tag/v0.0.3).
2. Нажмите значок карандаша **Edit**.
3. В поле **Attach binaries by dropping them here or selecting them** перетащите `AnomalyDX11AVX.exe` и соответствующий ему `AnomalyDX11AVX.pdb`.
4. Дождитесь завершения обеих загрузок и нажмите **Update release**.
5. Когда файлы добавлены, уберите из описания строку о предстоящей загрузке сборки.

Можно загрузить файлы напрямую или отдельными ZIP-архивами: например, `Quark-Engine-0.0.3-DX11-AVX2.zip` и `Quark-Engine-0.0.3-PDB.zip`. EXE и PDB должны происходить из одной сборки. ZIP **Source code**, который GitHub добавляет автоматически, содержит исходники.

## Создать 0.0.4 и следующие версии

1. Обновите код и `CHANGELOG.md`, задайте соответствующую версию в метаданных движка и подготовьте EXE с его PDB.
2. Откройте [создание нового релиза](https://github.com/quark-engine-stalker/quark-engine-stalker/releases/new).
3. В **Choose a tag** введите `v0.0.4` и выберите **Create new tag**. Для следующей версии используйте `v0.0.5` и так далее.
4. Выберите **Target: main**, содержащий исходники этой сборки.
5. Укажите название **Quark Engine 0.0.4 — Open Beta** и вставьте описание из [шаблона](docs/release-template.md), заполнив изменения этой версии.
6. Прикрепите EXE и PDB в поле загрузки бинарных файлов.
7. Для тестовой версии включите **This is a pre-release**. Для стабильной версии снимите эту отметку; её можно назначить **Latest**.
8. Нажмите **Publish release**. Если подготовка ещё идёт, используйте **Save draft**.

Каждой новой версии соответствует отдельный тег и релиз. Сохраняйте предыдущие выпуски, чтобы пользователи могли скачать их или выполнить откат.

Подробности интерфейса: [официальная инструкция GitHub](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).

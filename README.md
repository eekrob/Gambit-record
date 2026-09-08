# Gambit Record (`grecord`)

Gambit Record — GPLv3-инструмент для записи доказательств администраторами Gambit-RP.

Состоит из:

- `grecord.asi` — 32-битный ASI-плагин для GTA:SA / SA-MP;
- `GambitRecord.exe` — 64-битный worker для захвата окна, кодирования MP4 и загрузки видео.

Release-сборка загружает записи на общий YouTube-канал Gambit Record. Видео публикуются как `unlisted`.

Пользователю не нужны OAuth, Cloudflare, токены или собственный сервер. Установка выполняется через NSIS-инсталлятор.

Общий YouTube refresh token хранится только в Cloudflare Secrets и не передаётся клиенту.

[Политика конфиденциальности](PRIVACY.md) · [Условия использования](TERMS.md)

## Возможности

- `/grecord` — интерфейс с оформлением GAdmin, иконками и сворачиваемой навигацией:
  - запись;
  - загрузки;
  - настройки;
  - информация о программе.
- HUD показывает:
  - `REC`;
  - текущую цель наблюдения;
  - прогресс загрузки `UPLOAD %`.
- HUD по умолчанию снизу по центру. При открытом `/grecord` его можно перетащить мышью; при закрытом меню он не перехватывает ввод.
- Цель `/sp`, ID и ник игрока определяются через входящие SA-MP RPC.
- Поддерживается отслеживание:
  - `/ban`;
  - `/mute`;
  - `/jail`;
  - `/bmute`;
  - `/accban`.
- Наказание считается подтверждённым только после ответа сервера в течение 30 секунд.
- Поддерживаются наказания от другого администратора с суффиксом:
  `// Ник_текущего_админа`.
- После подтверждённого наказания можно:
  - остановить запись и загрузить её;
  - сохранить локально;
  - продолжить запись.
- Если запись не запущена, выводится предупреждение.
- Название видео:
  `Ник_администратора | YYYY-MM-DD | HH-MM-SS`.
- В описание добавляются:
  - сервер;
  - цель наблюдения;
  - команда;
  - причина;
  - период записи;
  - версия;
  - SHA-256.
- Загрузка:
  - resumable;
  - чанки по 8 МБ;
  - локальная очередь;
  - retry с увеличением задержки;
  - восстановление после перезапуска.
- Метки сохраняются в `grecord/markers.jsonl`.
- Аудиорежим:
  - звук процесса/системы;
  - микрофон;
  - без звука.
- Worker привязывается к PID `gta_sa.exe`.
- IPC — локальный named pipe с ограниченным ACL.
- Worker завершается вместе с игрой.
- Поддерживаются:
  - SA-MP 0.3.7 R1;
  - R3-1;
  - R5-1;
  - DL-1;
  - open.mp при совместимом ABI `samp.dll`.
- ASI активируется только на Gambit-RP.

Для совместимости со старым Evidence сохранены команды:

```text
/estart
/estop
/estatus
/esettings
```

## Интерфейс и тема GAdmin

В «Настройках» доступны масштаб HUD 75–200%, непрозрачность фона 20–100% и кнопка «Вернуть вниз по центру». Стандартные значения — 100% и 76%. Положение сохраняется после отпускания мыши относительно размеров экрана. При открытом меню HUD показывается и без записи; во время модального диалога перемещение отключено.

Настройки интерфейса хранятся отдельно в `grecord/ui.json`. Файл создаётся при первом изменении, заменяется атомарно и сохраняется при обновлении установки. Повреждённый файл загружается со стандартными значениями и уведомлением.

Переключатель «Использовать тему GAdmin» включён по умолчанию. Читается только общая палитра `internal.theme` из `gadmin/configuration/main.mpk` в папке игры: фон, текст и акцентные цвета в формате ABGR. GAdmin сохраняет изменения раз в пять минут; после обновления файла интерфейс подхватывает тему за 1–2 секунды без перезапуска. Индивидуальные настройки окон GAdmin не импортируются. При отсутствии файла используется встроенная тема, при ошибке чтения сохраняются последние корректные цвета и чтение повторяется. Состояние подключения видно в настройках. Шрифты Noto Sans и иконки Coolicons встроены в ASI.

В разделе «Загрузки» можно скопировать последнюю успешно полученную ссылку, в разделе «Запись» — открыть фактическую папку записей. IPC `status` дополнен совместимым полем `recording_directory` с абсолютным путём в UTF-8.

## Структура проекта

```text
grecord/                 ASI, ImGui, SA-MP hooks, тесты
recorder/                WGC/WASAPI/Media Foundation worker
broker/                  Cloudflare Worker + Durable Objects
installer/               NSIS installer и миграция Evidence
.github/workflows/       CI и release pipeline
```

## Сборка worker

Требования:

- Visual Studio 2022;
- Windows SDK;
- CMake;
- vcpkg.

```powershell
cmake --preset worker-ci
cmake --build --preset worker-ci
ctest --preset worker-ci
```

В локальной сборке `GRECORD_BROKER_KEY` пустой.

В release-сборке ключ добавляется через GitHub Actions Secret.

`BROKER_KEY` не даёт доступ к YouTube refresh token, но может быть извлечён из бинарника, поэтому брокер дополнительно использует лимиты запросов.

## Сборка ASI

ASI собирается только под x86.

Поддерживаются:

- MinGW GCC 15+;
- MSVC Win32.

```powershell
cmake -S grecord -B build-asi -A Win32
cmake --build build-asi --config Release
ctest --test-dir build-asi -C Release --output-on-failure
```

Для тестов конфигурации, восстановления темы и геометрии HUD включите `-DBUILD_TESTING=ON`. Дополнительные тесты настоящей отрисовки и мыши запускаются с `-DGRECORD_UI_RENDER_TESTS=ON` (нужен доступный D3D9 GPU). Они работают в скрытом окне без GTA, сохраняют PNG в `build-asi/Release/ui-preview` и проверяют встроенные шрифты, перетаскивание, сохранение положения, отключение ввода и границы экрана. Игровая совместимость с GAdmin требует отдельной проверки в GTA.

ImGui использует тот же pinned commit, что и GAdmin.

MinHook и `nlohmann/json` также зафиксированы в CMake.

## Установка

Release installer устанавливает:

```text
<GTA>/grecord.asi
<GTA>/GambitRecord.exe
<GTA>/grecord/config.json
```

`config.json` создаётся только при его отсутствии.

Если найден старый:

```text
moonloader/evidence.lua
```

он переименовывается в `.bak` с timestamp.

Старый config копируется в:

```text
grecord/migration
```

Существующие записи не удаляются.

Сборка установщика:

```powershell
./installer/build-installer.ps1 `
  -AsiPath ./build-asi/Release/grecord.asi `
  -WorkerPath ./build/bin/Release/GambitRecord.exe
```

Требуется NSIS 3.

## Broker

Этот раздел нужен только владельцу инфраструктуры.

Cloudflare Secrets:

```text
BROKER_KEY
YOUTUBE_CLIENT_ID
YOUTUBE_CLIENT_SECRET
YOUTUBE_REFRESH_TOKEN
```

YouTube refresh token использует scopes:

```text
youtube.upload
youtube.readonly
```

`youtube.upload` используется для загрузки.

`youtube.readonly` используется `/v1/channel` для определения канала.

Перед release необходимо:

1. Развернуть `broker/wrangler.toml`.
2. Добавить secrets через `wrangler secret put`.
3. Указать production HTTPS URL брокера.
4. Добавить `BROKER_KEY` в GitHub Actions как `GRECORD_BROKER_KEY`.
5. Проверить тестовую загрузку и `/v1/channel`.

OAuth credentials и refresh token не должны попадать:

- в клиент;
- в репозиторий;
- в CI-логи;
- в installer.

Durable Objects хранят только:

- resumable session URL;
- текущий offset;
- результат загрузки.

Видео брокер не сохраняет.

Сессии удаляются через 24 часа.

### API

```text
GET  /v1/channel
POST /v1/uploads
PUT  /v1/uploads/{id}
GET  /v1/uploads/{id}
POST /v1/uploads/{id}/cancel
```

Лимиты по умолчанию:

```text
8 МБ   — размер одного запроса
4 ГБ   — максимальный размер видео
10     — новых загрузок с одного IP в сутки
90     — новых загрузок глобально в UTC-сутки
```

Настройки находятся в `wrangler.toml`.

## Тесты

```powershell
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build-asi -C Release --output-on-failure

cd broker
pnpm check
pnpm test
```

Перед release вручную проверяются:

- R1;
- R3;
- R5;
- open.mp;
- совместимость с GAdmin;
- `/sp`;
- запуск записи;
- подтверждение наказания;
- загрузка;
- получение ссылки;
- отсутствие курсора при отображении HUD.

Игровой smoke test обязателен: ABI клиента и серверные сообщения могут изменяться.

## Лицензия

Проект распространяется по GNU GPL v3.

Часть идей и схем адресов/событий основана на [GAdmin](https://github.com/Vadim-Kamalov/GAdmin).

Использованный commit:

```text
c31749c02f3d76c1ab0f8ebf562c8dae0dc91152
```

Подробности: `THIRD_PARTY_NOTICES.md`.

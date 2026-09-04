# Gambit Record (`grecord`)

Gambit Record — GPLv3-инструмент записи доказательств для администраторов Gambit-RP. Он состоит из 32-битного ASI-плагина `grecord.asi`, который работает внутри GTA/SA-MP, и отдельного 64-битного worker `GambitRecord.exe`, который захватывает окно игры, кодирует MP4 и загружает его через контролируемый брокер.

> Все пользователи release-сборки загружают ролики на один общий YouTube-канал Gambit Record. Приватность ролика — `unlisted`: ролик доступен любому, у кого есть ссылка. Для непроверенного YouTube API-проекта Google может дополнительно принудительно ограничить видимость роликов.

## Возможности

- `/grecord`: вкладки «Запись», «Загрузки», «Настройки», «О программе» в тёмной ImGui-теме.
- HUD `REC`, цель наблюдения и `UPLOAD %` отрисовывается с `NoInputs` и не включает курсор.
- Настоящие входящие SA-MP RPC определяют цель `/sp`, ID и ник игрока.
- `/ban`, `/mute`, `/jail`, `/bmute`, `/accban` считаются подтверждёнными только после ответа сервера в течение 30 секунд. Поддерживается выдача другим администратором с суффиксом `// Ник_текущего_админа`.
- После подтверждения предлагается завершить и загрузить, сохранить локально либо продолжить. Без записи выводится предупреждение.
- Заголовок: `Ник_администратора | YYYY-MM-DD | HH-MM-SS`. Описание содержит сервер, цель, команду, причину, период, версию и SHA-256.
- 8-МБ resumable chunks, постоянная локальная очередь, повтор с увеличивающейся задержкой и восстановление после перезапуска.
- Метки важных моментов сохраняются в `grecord/markers.jsonl`.
- В настройках выбирается один аудиоисточник: звук процесса/системы, микрофон либо запись без звука.
- Worker привязан к PID `gta_sa.exe`, использует локальный named pipe с ограниченным ACL и завершается вместе с игрой.
- Поддерживаемые таблицы адресов: SA-MP 0.3.7 R1, R3-1, R5-1 и DL-1; open.mp поддерживается, когда его `samp.dll` совместим с одной из этих ABI.
- Работа ASI включается только на Gambit-RP.

Скрытые `/estart`, `/estop`, `/estatus`, `/esettings` оставлены на один переходный релиз. MoonLoader в новую поставку не входит.

## Структура

```text
grecord/                 32-bit ASI, ImGui, SA-MP hooks и unit-тесты
recorder/                64-bit WGC/WASAPI/Media Foundation worker
broker/                  Cloudflare Worker + Durable Objects
installer/               установщик и миграция старого MoonLoader варианта
.github/workflows/       PR-проверки и release pipeline
```

## Сборка worker

Нужны Visual Studio 2022, Windows SDK, CMake и vcpkg:

```powershell
cmake --preset worker-ci
cmake --build --preset worker-ci
ctest --preset worker-ci
```

`GRECORD_BROKER_KEY` намеренно пуст в обычной локальной сборке. Только release workflow внедряет общий ключ из GitHub Actions Secret. Этот ключ не раскрывает YouTube refresh token, но может быть извлечён из публичного бинарника; лимиты брокера остаются обязательной защитой.

## Сборка ASI

ASI всегда 32-битный. Поддерживается MinGW GCC 15+ (release CI) и MSVC Win32:

```powershell
cmake -S grecord -B build-asi -A Win32
cmake --build build-asi --config Release
ctest --test-dir build-asi -C Release --output-on-failure
```

Зависимости ImGui зафиксированы на том же commit, который использует GAdmin; MinHook и nlohmann/json также pinned в CMake.

## Установка

Release installer ставит только:

- `<GTA>/grecord.asi`;
- `<GTA>/GambitRecord.exe`;
- `<GTA>/grecord/config.json` при его отсутствии.

Существующий `moonloader/evidence.lua` переименовывается в timestamped `.bak`, а его config копируется в `grecord/migration`. Записи и старый каталог доказательств не удаляются.

## Настройка брокера

1. Создайте Cloudflare Worker и Durable Objects из `broker/wrangler.toml`.
2. Установите secrets командами `wrangler secret put`: `BROKER_KEY`, `YOUTUBE_CLIENT_ID`, `YOUTUBE_CLIENT_SECRET`, `YOUTUBE_REFRESH_TOKEN`.
3. Укажите реальный HTTPS URL в `config.example.json` перед release.
4. Добавьте тот же `BROKER_KEY` как GitHub Actions Secret `GRECORD_BROKER_KEY`.
5. Выполните пробную загрузку и `GET /v1/channel` перед тегом.

OAuth client id, client secret и refresh token не должны попадать в клиент, GitHub, логи CI или installer. Durable Objects хранят только URL resumable-сессии, смещение и результат; запись через сервер не сохраняется. Сессия удаляется alarm через 24 часа.

API:

- `GET /v1/channel`;
- `POST /v1/uploads`;
- `PUT /v1/uploads/{id}`;
- `GET /v1/uploads/{id}`;
- `POST /v1/uploads/{id}/cancel`.

По умолчанию: 8 МБ на запрос, 4 ГБ на ролик, 10 новых загрузок с IP и 90 глобально в UTC-сутки. Значения задаются в `wrangler.toml`.

## Проверка

```powershell
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build-asi -C Release --output-on-failure
cd broker
pnpm check
pnpm test
```

Перед выпуском вручную проверяются R1/R3/R5/open.mp, совместный запуск с GAdmin, `/sp` → запись → подтверждённое наказание → загрузка → ссылка, а также отсутствие курсора от HUD. Автотесты не заменяют игровой smoke test, потому что ABI стороннего клиента и серверные тексты могут меняться.

## Лицензия и GAdmin

Проект распространяется по GNU GPL v3. Использованные идеи и части схемы адресов/событий происходят из [GAdmin](https://github.com/Vadim-Kamalov/GAdmin) commit `c31749c02f3d76c1ab0f8f562c8dae0dc91152`. Подробности — в `THIRD_PARTY_NOTICES.md`.

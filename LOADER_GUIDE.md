# DotaFarm Loader — Инструкция

## Что это

Система для запуска до 5 Dota 2 ботов параллельно с одной машины. Боты сами создают пати, ищут матч, пикают героев и играют через инжект C++ DLL.

## Архитектура

```
┌─────────────────────────────────────────────────────┐
│ DotaFarmBootstrap.exe  (~50 KB, на сервере)         │
│  ├─ Скачивает DotaFarm.zip с v1per.tech             │
│  ├─ Распаковывает в %LOCALAPPDATA%\DotaFarm\        │
│  └─ Запускает DotaFarm.exe                          │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│ DotaFarm.exe           (orchestrator, x64 GUI)      │
│  ├─ License auth                                    │
│  ├─ Управление аккаунтами через GUI                 │
│  ├─ Запуск Steam+Dota инстансов                     │
│  ├─ Инжект Andromeda-Dota2-Base.dll                 │
│  └─ Мониторинг и логирование                        │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│ Andromeda-Dota2-Base.dll (внутри dota2.exe)         │
│  ├─ State machine (IN_MENU → FORMING_PARTY → ...)   │
│  ├─ GC interface hooks (party invite/accept/queue)  │
│  ├─ Bot brain (Lua scripting)                       │
│  └─ Hero pick + autobuy + bot logic                 │
└─────────────────────────────────────────────────────┘
```

## Установка для пользователя

### 1. Скачать бутстрап

```
https://v1per.tech/dota/DotaFarmBootstrap.exe
```

Один файл ~50 KB.

### 2. Запустить бутстрап (от админа)

Бутстрап автоматически:
- Проверит версию на сервере (`/dota/version.txt`)
- Скачает `DotaFarm.zip` если новая (~6 MB)
- Распакует в `%LOCALAPPDATA%\DotaFarm\`
- Удалит `config\farm.json` (пересоздаётся свежий)
- Запустит `DotaFarm.exe`

### 3. Активация (первый запуск)

```
[License Key]
[XXXXX-XXXXX-XXXXX-XXXXX]
[Activate]
```

После активации ключ сохраняется в `%LOCALAPPDATA%\DotaFarm\license.dat`.

### 4. Настройка аккаунтов

**Account Setup экран:**
- Поле ввода логина Steam → кнопка "Add" (или Enter)
- Так добавляются 1-5 аккаунтов
- Кнопка **"Start Steam Login"** → переход к Steam Login Wizard
- Кнопка **"Skip (already logged in)"** → если аккаунты уже залогинены раньше

**Steam Login Wizard:**
- Для каждого аккаунта по очереди:
  1. Кнопка "Open Steam" — открывает Steam с правильным AutoLoginUser
  2. Логинишься в Steam (вводишь пароль/2FA), включаешь "Remember password"
  3. Закрываешь Steam (X)
  4. Кнопка "Next" — переход к следующему аккаунту
- В конце "Finish Setup" — сохраняет `accounts.json`

После этого `steam_id` и `persona` подтягиваются автоматически из `loginusers.vdf`.

### 5. Dashboard

- Список ботов с чекбоксами **On/Off** (можно временно выключить аккаунт)
- Кнопка **"Start Farm"** — запуск всех включённых
- Кнопка **"Stop Farm"** — остановка
- Кнопка **"Reset Accounts"** (красная) — удаляет конфиг и BotSteam папки, начать сначала

## Что происходит при Start Farm

1. **Создание изоляции** для каждого бота:
   - `C:\BotProfiles\bot{N}\` — отдельный AppData (env vars `USERPROFILE`, `APPDATA`, `LOCALAPPDATA`, `TEMP` подменяются)
   - `C:\BotSteam\{N}\` — отдельная Steam-копия (junctions на оригинал + реальный `config/`)

2. **Запуск Steam+Dota** последовательно для каждого бота:
   - Реестр `AutoLoginUser` ставится на нужный аккаунт
   - Steam запускается из BotSteam dir с env redirect
   - `-applaunch 570 -master_ipc_name_override steam{N}` запускает Доту
   - Между запусками — kill `dota_singleton_mutex` через `handle64.exe`

3. **Ожидание `client.dll`** для каждой Доты (до 2 минут)

4. **Инжект `Andromeda-Dota2-Base.dll`** через LoadLibrary

5. **State Machine в DLL**:
   - `IN_MENU` → ждёт client_version (cv) детект
   - **Leader**: `FORMING_PARTY` → шлёт `InviteToParty` (4501) для всех members
   - **Members**: ждут invite через `SOCacheSubscribed` (msg 24) с `CSODOTAPartyInvite` (type_id=2006)
   - **Member accept** двойной: GC 4503 (вступает в пати) + PostMessage Enter в Дoту (закрывает попап)
   - `QUEUING` → `StartFindingMatch` (7033)
   - `MATCH_FOUND` → `ReadyUp` (7070) + Panorama JS `DOTAMatchReadyAccept`
   - `HERO_SELECTION` → `dota_select_hero` через console
   - `GAME_IN_PROGRESS` → Lua bot brain тикает каждые 100мс

## Файлы и пути

```
%LOCALAPPDATA%\DotaFarm\          # установка от бутстрапа
├─ DotaFarm.exe                   # orchestrator
├─ Andromeda-Dota2-Base.dll       # инжектируемая DLL чита
├─ handle64.exe                   # для kill mutex
├─ version.txt                    # текущая версия
├─ license.dat                    # сохранённый ключ
├─ debug_<PID>.log                # лог DLL для каждой Доты
├─ DotaFarm.log                   # лог orchestrator
├─ data/                          # game data (heroes, items, abilities)
├─ scripts/bots/                  # Lua скрипты ботов (Customize, BotLib...)
└─ config/
   ├─ accounts.json               # логины + steam_id (от GUI)
   └─ farm.json                   # героев, регион, mode, dll_path

C:\BotProfiles\botN\              # изоляция AppData (создаётся orchestrator'ом)
├─ AppData\Roaming\
├─ AppData\Local\
└─ Temp\

C:\BotSteam\N\                    # micro-copy Steam (junctions + real config)
├─ steam.exe                      # symlink на реальный Steam
├─ Steam.cfg                      # BootStrapperInhibitAll=enable
├─ steamapps                      # junction
└─ config\
   ├─ loginusers.vdf              # СВОЯ для каждого бота — отдельный логин
   ├─ libraryfolders.vdf          # копия из реального Steam (знает все библиотеки)
   └─ config.vdf
```

## Деплой обновлений (для разработчика)

```bash
# 1. Собрать orchestrator (если изменился)
cd tools/dota2/orchestrator
cmake --build build --config Release

# 2. Собрать DLL чита (если изменилась)
cd /c/temp/andromeda_src/Andromeda-Dota2-Base
"/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
    Andromeda-Dota2-Base.vcxproj -p:Configuration=Release -p:Platform=x64

# 3. Скопировать артефакты в пакет
cp build/Release/DotaFarm.exe /c/temp/DotaFarm/
cp /c/temp/andromeda_src/Andromeda-Dota2-Base/x64/Release/Andromeda-Dota2-Base.dll /c/temp/DotaFarm/

# 4. Создать zip + залить
cd /c/temp/DotaFarm
powershell.exe -NoProfile -Command "Compress-Archive -Path @('DotaFarm.exe','Andromeda-Dota2-Base.dll','handle64.exe','README.txt','data','scripts') -DestinationPath 'C:\temp\DotaFarm_deploy.zip' -Force"
scp /c/temp/DotaFarm_deploy.zip eft-deploy:/data/static/dota/DotaFarm.zip
echo "2026.04.08.0027" | ssh eft-deploy 'cat > /data/static/dota/version.txt'
```

Бутстрап у пользователя при следующем запуске увидит новую `version.txt`, скачает новый zip, удалит `farm.json` (пересоздастся), запустит DotaFarm.

## Сервер (eft-deploy)

```
/data/static/dota/
├─ DotaFarm.zip               # пакет (DLL + EXE + scripts + data)
├─ DotaFarmBootstrap.exe      # бутстрап
└─ version.txt                # текущая версия (e.g. "2026.04.08.0027")
```

Caddy route в `/opt/eft/Caddyfile`:
```
handle /dota/* {
    uri strip_prefix /dota
    root * /data/static/dota
    file_server
}
```

## Известные проблемы и решения (из dev_log)

### "Дoта не находится" → Steam exe в нестандартном месте
DotaFarm авто-детектит `SteamPath` через `HKCU\Software\Valve\Steam\SteamExe`, фильтруя BotSteam пути. Fallback на стандартные пути (`C:\Program Files (x86)\Steam`, `D:\Steam`, и т.д.).

### "Account picker появляется при Start Farm"
Решение: per-bot Steam через junction'ы `C:\BotSteam\N\` с реальным `config/loginusers.vdf` для каждого бота. Каждый Steam знает только один аккаунт.

### "Game files corrupted" при запуске Дoты из BotSteam
Решение: копировать `libraryfolders.vdf` из реального Steam в BotSteam config — Steam сразу знает где установлены игры (`D:\SteamLibrary` и т.д.).

### "Steam.cfg симлинк" → автообновление сбрасывает конфиг
Решение: Steam.cfg создаётся как **реальный файл** с `BootStrapperInhibitAll=enable` (не симлинк).

### "client_version=0, GC отбрасывает invite"
- Скан incoming GC сообщений на тип 2528 (`CMsgClientVersionUpdated`) — содержит cv в field 1
- Fallback hardcoded `cv=6760` через 60 секунд если ничего не задетектилось

### "GC accept (4503) не закрывает попап"
Решение: dual approach
1. **GC 4503** — вступает в пати (фактический accept)
2. **PostMessage Enter** на окно Дoты — закрывает попап (`AcceptButton` имеет `defaultfocus`)
   - Window finding: `EnumWindows` + filter по PID + `IsWindowVisible` + title содержит `"Dota"`
   - Перед нажатием: `hideconsole` (иначе Enter уходит в консоль)
   - `SetForegroundWindow` + `PostMessageW(WM_KEYDOWN, VK_RETURN, 0x001C0001)` + `PostMessageW(WM_KEYUP, VK_RETURN, 0xC01C0001)`

### "Логи теряются" — оба бота пишут в один файл
Решение: per-PID лог `debug_<PID>.log`.

## Версия

Текущая: **v0027** (2026-04-08)
- type_id=2006 для CSODOTAPartyInvite (раньше 2003/2004)
- Dual accept (GC 4503 + Enter keypress)
- Title-based window finding ("Dota")
- per-PID debug logs

# Dota 2 Farm — Handoff (2026-04-18)

## Текущая задача

**Phase A (state observability) — сделана и проверена в живую**:
- GSI HTTP listener в orchestrator (порт 3477), cfg установлен в Dota.
- Status JSON из DLL расширен **schema 2**: GC state (party/queue/match), полный EDOTAGameState, heartbeat, own steam_id, connection health.
- Дашборд: GSI status строка, Party panel (общая инфа о пати), Operational status строки.
- Hot-inject fix: OnEnter_InMenu больше не разрушает живую пати с не-нашими steam_ids.

**Phase B (memory SOCache pull) — следующая сессия.** План в `prompt_next.md`. Закроет hot-inject слепое пятно для menu/queue/party (там где GSI не работает).

**Полный отчёт сессии**: `SESSION_2026-04-18.md`.

## Версия артефактов

- DLL: `Andromeda-Dota2-Base.dll` md5 **`e167b136cd478d3bcd9ab25966b9e744`** (~2 MB, x64 Release)
- EXE: `DotaFarm.exe` md5 **`1f351542570babe2ea9ca4c5a749b7d8`**
- Лежат в `C:\temp\DotaFarm\` и `C:\temp\andromeda\`

## Не задеплоено

`scp /c/temp/DotaFarm_deploy.zip eft-deploy:/data/static/dota/DotaFarm.zip` НЕ выполнялся в этой сессии. Если нужно — собрать zip и залить:
```bash
cd /c/temp/DotaFarm
powershell.exe -NoProfile -Command "Compress-Archive -Path @('DotaFarm.exe','Andromeda-Dota2-Base.dll','handle64.exe','README.txt','data','scripts') -DestinationPath 'C:\temp\DotaFarm_deploy.zip' -Force"
scp /c/temp/DotaFarm_deploy.zip eft-deploy:/data/static/dota/DotaFarm.zip
echo "2026.04.18.0001" | ssh eft-deploy 'cat > /data/static/dota/version.txt'
```

## Деплой (стандартный флоу)

```bash
# 1. Сборка DLL
powershell.exe -Command "& 'C:\temp\build_andromeda.bat'"

# 2. Сборка orchestrator
"/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build "tools/dota2/orchestrator/build" --config Release

# 3. Копирование артефактов
cp /c/temp/andromeda_src/Andromeda-Dota2-Base/x64/Release/Andromeda-Dota2-Base.dll /c/temp/DotaFarm/
cp /c/temp/andromeda_src/Andromeda-Dota2-Base/x64/Release/Andromeda-Dota2-Base.dll /c/temp/andromeda/
cp "tools/dota2/orchestrator/build/Release/DotaFarm.exe" /c/temp/DotaFarm/

# 4. Zip + scp + version.txt (см. блок выше)
```

## Архитектура состояния (после Phase A)

```
                   ┌──────────────────────┐
                   │     orchestrator     │
                   │      (DotaFarm)      │
                   └──┬────────┬──────────┘
                      │        │
  GSI HTTP POST :3477 │        │ status_<PID>.json (schema 2)
  ── полное in-game ──┘        │ ── menu/queue/party/heartbeat ──
       ▲                       │
       │                       ▼
  ┌────┴────┐            ┌──────────┐
  │ dota2.exe│           │ DLL inj. │
  │ (Valve)  │           │(Andromeda)│
  └─────────┘            └──────────┘
   через cfg              через MinHook на
   gamestate_integration  RetrieveMessage + SendMessage
                          (плюс gcState из CGCMessageHandler)
```

**Источники истины:**
- **Game Rules** (CGameState через FullScan) — состояние матча, hp/maxHp локального игрока.
- **GSI HTTP** — то же что Game Rules + abilities/items/draft/buildings (нативное от Valve, без реверса).
- **GC SOCache** (через хук RetrieveMessage) — party/lobby/invite/queue. **Слабое звено**: при hot-inject слепы → нужен Phase B.

## Ключевые файлы (новые/изменённые)

### Orchestrator
- `tools/dota2/orchestrator/src/gsi_server.h/.cpp` — НОВЫЙ. HTTP listener.
- `tools/dota2/orchestrator/src/gsi_install.h/.cpp` — НОВЫЙ. Авто-установка cfg.
- `tools/dota2/orchestrator/src/orchestrator.h/.cpp` — расширены `BotInstance` + `EnsureGsi/ReadGsiSnapshots/GetGsiStatus`.
- `tools/dota2/orchestrator/src/gui.cpp` — Party panel + Operational status строки.
- `tools/dota2/orchestrator/CMakeLists.txt` — `gsi_*.cpp` + `ws2_32.lib`.

### DLL
- `Andromeda/BotFarm/CBotFarmClient.cpp` — `WriteStatusFile` schema 2 (через nlohmann/json).
- `Andromeda/BotFarm/StateMachine/CBotStateMachine.cpp` — `OnEnter_InMenu` не разрушает чужую пати.

### Утилиты
- `tools/dota2/gsi_test_listener.py` — НОВЫЙ. Standalone Python listener для отладки GSI.

## Что работает после Phase A

- Orchestrator поднимает GSI HTTP server на 127.0.0.1:3477 при старте (даже до Start Farm).
- Если дота уже запущена — `EnsureGsi()` найдёт её и положит cfg. Но Дота читает cfg ТОЛЬКО при старте → первый раз нужен рестарт клиента (или `gamestate_integration_load` в консоли доты).
- Когда дота в матче (любом — Demo Hero, бот-матч, реальный) — пакеты GSI летят ~1/сек, заполняют snapshot per steam_id.
- DLL пишет полный gcState в `status_<PID>.json` каждую секунду. Orchestrator парсит schema 2.
- Дашборд показывает: общую party panel, операционные индикаторы (DLL age / GC msgs / party size / queue / match found / full game state) для каждого бота.

## Что НЕ закрыто (план Phase B и далее)

- Hot-inject в menu / queue / party / match-found — мы слепы к SOCache до момента инжекта. **Phase B**.
- Старые 5 проблем из старого `prompt_next.md` (попап, авто-accept матча, мид-АФК, скиллы, сайды-в-лесу) — с полной observability будут решаться быстрее.

## Полезные команды

```bash
# Локальный listener (без orchestrator)
python tools/dota2/gsi_test_listener.py

# Проверить cfg
ls -la "/d/SteamLibrary/steamapps/common/dota 2 beta/game/dota/cfg/gamestate_integration/"

# В Dota консоли: gamestate_integration_status / gamestate_integration_load

# PID Dota
powershell.exe -Command "Get-Process dota2 -ErrorAction SilentlyContinue | Select-Object Id"
```

## История prior sessions

- `SESSION_2026-04-18.md` — Phase A (текущая, обзор GSI)
- `SESSION_2026-04-17.md` — попытки фикса попапа/accept матча (Проблемы 1-5)
- v0029-v0045 история фиксов в старом `HANDOFF.md` (см. git history)

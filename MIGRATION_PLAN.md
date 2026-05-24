# Dota 2 Bot Farm — Миграция Python → C++ Internal DLL

## Мастер-план (10 фаз, ~15-22 сессий)

**Цель**: полная миграция Python бот-фермы на C++ internal DLL (Andromeda-Dota2-Base).
**Зачем**: продажа (один .dll + VMProtect), скорость (0ms entity read, direct calls), надёжность (auto-offsets).
**База**: Andromeda v1.0.3 — собран, заинжекчен, все 17 паттернов живы (2026-03-26).

---

## Архитектура

```
┌─────────────────── dota2.exe ───────────────────┐
│  DotaBot.dll (internal, manual mapped)          │
│  ├── Andromeda Base (patterns, schema, hooks)   │
│  ├── EntityCache (OnAddEntity driven, 0ms)      │
│  ├── GameState (gamerules, local player)        │
│  ├── Commands (direct calls, no shellcode!)     │
│  │   ├── CCommandBuffer::AddText               │
│  │   ├── PrepareUnitOrders                      │
│  │   └── CUIEngine::RunScript (Panorama JS)    │
│  ├── GC (protobuf classes, direct vtable)       │
│  │   ├── ISteamGameCoordinator::SendMessage     │
│  │   └── RetrieveMessage hook                   │
│  ├── StateMachine (menu→queue→pick→game→end)    │
│  ├── BotBrain (embedded Lua via sol2)           │
│  │   └── 40+ mocked API functions              │
│  ├── GameData (JSON: heroes, abilities, items)  │
│  └── ImGui Menu (debug + config)                │
└─────────────────────────────────────────────────┘
         ↑ inject (BlackBone manual map)
┌────────────────────────────────────────┐
│  Python Launcher (external, остаётся)  │
│  ├── Multi-instance (AppData redirect) │
│  ├── Mutex close (handle64.exe)        │
│  ├── Config generation per bot         │
│  └── Health monitoring / restart       │
└────────────────────────────────────────┘
```

---

## Что переиспользуем из Andromeda (без изменений)

- CBasePattern (паттерн-сканер, 6 режимов) — все 17 паттернов работают
- CSchemaOffset (runtime schema resolver) — auto-adaptive к обновам
- CGameEntitySystem (chunk lookup, handle resolve)
- Все хуки: OnCreateMove, OnAddEntity/Remove, DX11 Present
- CFunctionList (GetLocalPlayerController, CUserCmd access)
- **91 protobuf .pb.h/.pb.cc** (25MB, все GC messages Доты!)
- libprotobuf.lib, libMinHook.lib
- ImGui + DX11 backend
- Math (Vector3, QAngle, Matrix), CHandle, XorStr, DevLog, CrashLog

## Что добавляем

- **sol2** (header-only) — Lua ↔ C++ binding
- **Lua 5.4** (static lib) — runtime для бот-скриптов
- **nlohmann/json** (header-only) — JSON конфиги и game data

---

## Phase 0: Scaffold — форк и структура ✅ DONE (2026-03-26)
**Сессий: 1 | Сложность: Low**

- ~~Скопировать Andromeda в `tools/dota2/dll/`~~ → работаем в `C:\temp\andromeda_src\`
- ✅ Создать структуру `BotFarm/` (GameState, Commands, GC, StateMachine, BotBrain, Data, Settings)
- ✅ Убрать camera hack (CAndromedaClient, CAndromedaMenu удалены)
- ✅ CAndromedaGUI оставлен (DX11/ImGui инфраструктура, не камера)
- ✅ sol2 v3.3.1, lua54.lib (x64/MT), nlohmann/json v3.11.3 — добавлены (Phase 6+7)
- ✅ CBotFarmClient заменяет CAndromedaClient
- ✅ Config: name="DotaBot", version="0.1.0"
- ✅ **Тест**: билд 0 errors + инжект OK + `[+] BotFarm loaded v0.1.0` + LocalPlayer: 1, Screen: 1920x1080

**Архитектура утверждена**: гибрид C++ (движок) + Lua (мозги бота, Phase 7)

**Файлы**: DllLauncher.cpp (edit), Hook_OnCreateMove.cpp (edit), CAndromedaGUI.cpp (edit), Config.hpp (edit), .vcxproj (edit), BotFarm/CBotFarmClient.hpp/cpp (new), BotFarm/Settings/Settings.hpp (new)

---

## Phase 1: Entity Cache — hook-driven tracking ✅ DONE (2026-03-26)
**Сессий: 1 | Сложность: Medium | Зависит от: Phase 0**

### Реализовано

- ✅ `CEntityCache`: `std::unordered_map<int, EntityData>`, заполняется в OnAddEntity/OnRemoveEntity
- ✅ `EntityData` struct: pEntity, handle, entityIndex, type, designerName + динамические поля
- ✅ Per-tick refresh в OnCreateMove (~30Hz): team, hp, maxHp, lifeState, position, level, mana, unitState64, damage
- ✅ Классификация по `npc_dota_` префиксу: HERO / CREEP / TOWER / BUILDING / COURIER / OTHER
- ✅ `FullScan()` для hot inject (итерация всех существующих entity)
- ✅ Thread-safe (std::mutex для render/game thread)
- ✅ Debug overlay: "DotaBot | Entities: N, Heroes: M" зелёным

### Schema offsets добавлены

```cpp
// CGameSceneNode (новый класс)
SCHEMA_OFFSET("CGameSceneNode", "m_vecAbsOrigin", m_vecAbsOrigin, Vector3)

// C_BaseEntity — 4 новых поля
SCHEMA_OFFSET("C_BaseEntity", "m_iHealth", m_iHealth, int32)
SCHEMA_OFFSET("C_BaseEntity", "m_iMaxHealth", m_iMaxHealth, int32)
SCHEMA_OFFSET("C_BaseEntity", "m_lifeState", m_lifeState, uint8)
SCHEMA_OFFSET("C_BaseEntity", "m_pGameSceneNode", m_pGameSceneNode, CGameSceneNode*)

// C_DOTA_BaseNPC — 10 полей
SCHEMA_OFFSET("C_DOTA_BaseNPC", "m_iCurrentLevel/m_flMana/m_flMaxMana/m_iszUnitName/...")

// C_DOTA_BaseNPC_Hero — 5 полей
SCHEMA_OFFSET("C_DOTA_BaseNPC_Hero", "m_iCurrentXP/m_iPlayerID/m_flStrength/...")
```

### Тест (verified live, 1v1 bot match, 2026-03-26)

- ✅ `[EntityCache] Add: npc_dota_hero_terrorblade` — героев трекает
- ✅ `[EntityCache] Add: npc_dota_creep_lane` — крипы spawn
- ✅ `[EntityCache] Remove: npc_dota_creep_lane` — крипы die
- ✅ `[EntityCache] Add: npc_dota_tower` — башни
- ✅ `[EntityCache] Add: npc_dota_courier` — курьеры
- ✅ Фильтр `npc_dota_` — нет ложных `creep_piercing`, `courier_take_stash_items`
- ✅ 89 entity events за 2+ мин, 0 крашей
- ✅ team=0 в логе OnAddEntity — нормально (ещё не инициализирован), OnTick обновляет

### Баги / заметки

- `team=0` при add — team заполняется engine'ом ПОСЛЕ OnAddEntity. OnTick() корректно обновляет.
- `FullScan: found 0` на main menu — корректно, entity ещё нет.
- DLL output path: MSBuild для .vcxproj → `Andromeda-Dota2-Base/x64/Release/`, но инжектор ищет в `andromeda_src/x64/Release/` (solution output). **Нужно копировать или билдить .sln.**

### Файлы (7 изменений)

| Файл | Действие |
|------|----------|
| `Dota2/SDK/Types/CEntityData.hpp` | +CGameSceneNode, +4 поля C_BaseEntity, +10 C_DOTA_BaseNPC, +5 C_DOTA_BaseNPC_Hero |
| `BotFarm/GameState/CEntityCache.hpp` | NEW — EntityType enum, EntityData struct, CEntityCache class |
| `BotFarm/GameState/CEntityCache.cpp` | NEW — ClassifyEntity, OnAdd/Remove, OnTick, FullScan, accessors |
| `Dota2/Hook/Hook_OnAddEntity.cpp` | → GetEntityCache()->OnAddEntity(), убран DEV_LOG спам |
| `Dota2/Hook/Hook_OnRemoveEntity.cpp` | → GetEntityCache()->OnRemoveEntity(), убран DEV_LOG спам |
| `BotFarm/CBotFarmClient.cpp` | +FullScan в OnInit, +OnTick в OnCreateMove, +debug overlay в OnRender |
| `Andromeda-Dota2-Base.vcxproj` | +CEntityCache.cpp/.hpp |

---

## Phase 2: Game State — gamerules + local player ✅ DONE (2026-03-26)
**Сессий: 1 | Сложность: Low-Medium | Зависит от: Phase 1 ✅**

### Реализовано

- ✅ `CGameState` синглтон: gamerules detection + local player tracking, per-tick refresh в OnCreateMove
- ✅ **Вариант A** выбран: gamerules детектится в Hook_OnAddEntity/OnRemoveEntity **до** EntityCache (designerName == `"dota_gamerules"`)
- ✅ `C_DOTAGamerules` + `C_DOTAGamerulesProxy` — `SCHEMA_OFFSET_CUSTOM` (hardcoded offsets, класс скрыт от SchemaSystem runtime)
- ✅ `EDOTAGameState` enum (0..13 + NONE=-1), `GameStateToString()` для overlay
- ✅ `LocalPlayerData`: controller, hero, playerID, team, health, isAlive, position
- ✅ `FullScan()` для hot inject — находит gamerules proxy среди существующих entity
- ✅ Thread-safe (std::mutex), как CEntityCache
- ✅ 3-строчный debug overlay: entities (green) + state/pick/mode (yellow) + team/PID/hero/pos (cyan)
- ✅ `m_nPlayerID` добавлен в `C_DOTAPlayerController`
- ⏭️ `m_flGameTime` пропущен — НЕ существует в C_DOTAGamerules, добавим позже через CGlobalVars/ConVar

### Schema offsets (verified via dump)

```cpp
// C_DOTAGamerules — SCHEMA_OFFSET_CUSTOM (hidden from SchemaSystem)
m_nGameState           = 0x007C  // int32, DOTA_GAMERULES_STATE_*
m_nHeroPickState       = 0x0080  // int32
m_flStateTransitionTime = 0x0088 // float
m_iGameMode            = 0x00E4  // int32 (ВАЖНО: m_iGameMode, НЕ m_nGameMode!)

// C_DOTAGamerulesProxy
m_pGameRules           = 0x05F0  // C_DOTAGamerules*

// C_DOTAPlayerController (добавлено)
m_nPlayerID — SCHEMA_OFFSET (auto-resolved)
```

### Overlay (3 строки)

```
DotaBot | Entities: 42, Heroes: 10         ← green
State: HERO_SELECTION | PickState: 3 | Mode: 1  ← yellow
Team: 2 | PID: 0 | Hero: ALIVE | Pos: 1234,5678,128  ← cyan
```

### Файлы (7 изменений)

| Файл | Действие |
|------|----------|
| `Dota2/SDK/Types/CEntityData.hpp` | +C_DOTAGamerules (CUSTOM), +C_DOTAGamerulesProxy (CUSTOM), +m_nPlayerID |
| `BotFarm/GameState/CGameState.hpp` | NEW — EDOTAGameState enum, LocalPlayerData, CGameState class |
| `BotFarm/GameState/CGameState.cpp` | NEW — OnGameRulesFound/Removed, OnTick, FullScan, RefreshGameRules/LocalPlayer |
| `Dota2/Hook/Hook_OnAddEntity.cpp` | +gamerules detection before EntityCache |
| `Dota2/Hook/Hook_OnRemoveEntity.cpp` | +gamerules removal before EntityCache |
| `BotFarm/CBotFarmClient.cpp` | +GameState tick + FullScan + 3-line overlay |
| `Andromeda-Dota2-Base.vcxproj` | +CGameState.cpp/.hpp |

### Заметки

- `m_flGameTime` НЕ существует в C_DOTAGamerules — `m_flStateTransitionTime` есть, но это не game clock
- `m_iGameMode` (i prefix), НЕ `m_nGameMode` (n prefix) — верифицировано в schema dump
- `C_DOTAGamerules` не доступен через SchemaSystem runtime → все offsets hardcoded
- Gamerules proxy → `m_pGameRules` pointer перечитывается каждый tick (может быть set после создания proxy)
- Vector3 в проекте: `m_x`, `m_y`, `m_z` (не `x`, `y`, `z`)

---

## Phase 3: Commands — прямые вызовы ✅ DONE (2026-03-26)
**Сессий: 1 | Сложность: Medium | Зависит от: Phase 2 ✅**

### Реализовано

- ✅ `CConsoleCmd`: hardcoded RVA для CCommandBuffer (engine2+0x8CF9E8) + AddText (tier0+0x5BC60), verify +0x8028==0x400
- ✅ `CPrepareOrders`: AOB `4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08` через CBasePattern (SEARCH_TYPE_NONE)
- ✅ 8 convenience wrappers: MoveTo, AttackMove, AttackTarget, Stop, CastPosition/Target/NoTarget, TrainAbility
- ✅ EDOTAUnitOrder enum (17 значений), EOrderIssuer enum (4 значения)
- ⏭️ `CConVarSystem` — отложен (не нужен для Phase 5 StateMachine)
- ✅ Hot-reload: END hotkey — `MH_DisableHook(MH_ALL_HOOKS)` + WndProc restore (без ImGui cleanup — leak safe)

### Тест (verified live, 2026-03-26)

```
[CConsoleCmd] Init OK: buf=0x00007FF9DCCDF9E8 addtext=0x00007FF9DCDDBC60
[PrepareOrders] Init OK: fn=0x00007FF9C73C72A0
```
- ✅ Overlay 4-я строка: "Cmd: OK | Orders: OK" (magenta)
- ✅ END hotkey: overlay пропадает, игра работает, реинжект без рестарта

### Файлы (9 изменений)

| Файл | Действие |
|------|----------|
| `BotFarm/Commands/CConsoleCmd.hpp/cpp` | NEW — Execute(), IsReady(), hardcoded RVA + verify |
| `BotFarm/Commands/CPrepareOrders.hpp/cpp` | NEW — SendOrder(), 8 wrappers, enums |
| `Dota2/SDK/CFunctionList.hpp/cpp` | +PrepareUnitOrders pattern |
| `BotFarm/CBotFarmClient.cpp` | +Commands init + overlay line 4 |
| `AndromedaClient/CAndromedaGUI.cpp` | +END hotkey (safe unload) |
| `Andromeda-Dota2-Base.vcxproj` | +4 files |

---

## Phase 4: GC Messages — protobuf + ISteamGameCoordinator ✅ DONE (2026-03-27)
**Сессий: 1 | Сложность: High | Зависит от: Phase 0 ✅**

### Реализовано

- ✅ `CGCInterface`: `SteamInternal_FindOrCreateUserInterface(hUser, "SteamGameCoordinator001")` + SteamID
- ✅ `CGCMessages`: 7 typed senders через compiled protobuf (CMsgStartFindingMatch, CMsgReadyUp, etc.)
- ✅ `CGCMessageHandler`: **shellcode VMT hook** на RetrieveMessage — перехват incoming GC сообщений
- ✅ Send verified: F7 → StartFindingMatch → Dota UI показывает "Поиск..."
- ✅ Receive verified: `Msgs: N` растёт в overlay (shellcode counter + body logging)
- ✅ Protobuf parsing: StartFindingMatchResult (7034), ReadyUpStatus (7170), SOCacheSubscribed (24)

### Критические находки

1. **`SteamInternal_FindOrCreateUserInterface`** — единственный правильный способ. `ISteamClient::GetISteamGenericInterface` (vtable[12]) возвращает ДРУГОЙ объект с другой vtable → send работает, receive нет.
2. **Manual-mapped DLL код не исполняется** через vtable вызовы (Steam SEH молча глотает ACCESS_VIOLATION). Решение: весь detour как **x64 shellcode в VirtualAlloc(PAGE_EXECUTE_READWRITE)** — идентично gc.py.
3. **OnCreateMove не вызывается в main menu** → GC processing через OnRender.
4. **Polling бесполезен** — IsMessageAvailable всегда false для нашего интерфейса. Только VMT hook на объекте работает.
5. **Log buffer**: shellcode пишет last message в shared GCHookLog, DLL читает из OnRender (ProcessPending).

### GC Message Format
- **Send**: `[msgType|0x80000000 (4)][headerSize=0 (4)][protobuf body]`
- **Receive** (в буфере RetrieveMessage): `[headerSize (4)][header N bytes][protobuf body]`
- **ready_up_key**: `lobby_id ^ ~(account_id | account_id << 32)`

### Тест (verified live, 2026-03-27)

```
[CGCInterface] Init OK: gc=0x... steamID=76561198725850781 accountID=765585053
[CGCMessageHandler] Shellcode 140 bytes, VMT hook installed
[GC] StartFindingMatch: regions=0x80 modes=0x2  ← F7 hotkey
Msgs: N (растёт в overlay)                       ← shellcode VMT hook работает
```

### Файлы (8 изменений)

| Файл | Действие |
|------|----------|
| `BotFarm/GC/CGCInterface.hpp/cpp` | NEW — ISteamGameCoordinator + SteamID + SendGCMessage |
| `BotFarm/GC/CGCMessages.hpp/cpp` | NEW — EGCMsgID enum + 7 senders (protobuf) |
| `BotFarm/GC/CGCMessageHandler.hpp/cpp` | NEW — shellcode VMT hook + GCHookLog + protobuf parsers |
| `BotFarm/CBotFarmClient.cpp` | +GC init, +VMT hook, +ProcessPending, +overlay line 5, +F7/F8 |
| `Andromeda-Dota2-Base.vcxproj` | +6 files |

### Нерешённое (minor, не блокирует Phase 5)

- **Single-message log buffer**: shellcode хранит только последнее сообщение. Если 2 сообщения приходят между кадрами рендера, первое теряется. Решение: ring buffer (TODO если понадобится).
- **StopFindingMatch** не работает с Valve update ~2026-03-25 (протокол отмены изменён).

---

## Phase 5: State Machine — автоматический flow ✅ DONE (2026-03-27)
**Сессий: 1 | Сложность: Medium | Зависит от: Phases 2, 3, 4 ✅**

### Реализовано

- ✅ `CBotStateMachine`: singleton, enum + methods (не virtual IState), 7 состояний
- ✅ Тик в OnRender (10Hz, steady_clock) — OnCreateMove не вызывается в main menu
- ✅ Hot inject detection: first tick проверяет gameState и прыгает в правильное состояние
- ✅ Disconnect detection: HERO_SELECTION/PRE_GAME/GAME_IN_PROGRESS → IN_MENU при gameState == NONE
- ✅ F5 toggle auto-queue, F6 force reset, overlay line 6 (orange)
- ✅ Settings: 13 настроек (region, mode, hero, timeouts)
- ✅ `ResetMatchState()` в CGCMessageHandler
- ⏭️ PARTY_FORMING state — Phase 9 (multi-instance)
- ⏭️ BotBrain в GAME_IN_PROGRESS — Phase 7 (Lua)

### Transition table

```
IN_MENU → QUEUING (autoQueue + 15s delay)
IN_MENU → HERO_SELECTION (hot inject)
QUEUING → MATCH_FOUND (GC matchFound + lobbyID)
QUEUING → HERO_SELECTION (race: gameState already advanced)
MATCH_FOUND → HERO_SELECTION (gameState >= HERO_SELECTION)
HERO_SELECTION → PRE_GAME (gameState >= STRATEGY_TIME)
PRE_GAME → GAME_IN_PROGRESS (gameState >= 5)
GAME_IN_PROGRESS → POST_GAME (gameState >= POST_GAME)
POST_GAME → IN_MENU (disconnect + gameState == NONE)
Any game state → IN_MENU (disconnect detected: gameState == NONE/INIT)
```

**Тест**: ✅ overlay, ✅ F5 toggle, ✅ disconnect detection (verified live).

**Файлы**: CBotStateMachine.hpp/cpp (new), Settings.hpp (edit), CGCMessageHandler.hpp/cpp (edit), CBotFarmClient.cpp (edit), .vcxproj (edit)

---

## Phase 6: Game Data + Ability/Item System ✅ DONE (2026-03-27)
**Сессий: 1 | Сложность: Medium | Зависит от: Phase 1 ✅**

### Реализовано

- ✅ `CGameDataDB`: nlohmann/json v3.11.3, загрузка npc_heroes/abilities/items.json из `C:\temp\andromeda\data\`
- ✅ Level-aware stat lookup (`ParseLevelValues` + `PickLevelValue`) — парсит "21 18 15 12", выбирает по уровню
- ✅ AbilityValues (nested dict с per-level scaling) — полный парсинг
- ✅ Behavior flags — 20 флагов, `GetAbilityCastType()` → "target"/"position"/"no_target"
- ✅ Hero merge (base template + override) — как в Python версии
- ✅ Extend EntityCache: `RefreshAbilities()` — m_vecAbilities → resolve CHandle → C_DOTABaseAbility → 9 полей
- ✅ Extend EntityCache: `RefreshItems()` — m_Inventory → m_hItems → C_DOTA_Item → 6 полей
- ✅ `CNetworkUtlVectorBase<T>` — pointer-based network vector (abilities)
- ✅ `CNetworkUtlVectorInline<T>` — inline data network vector (items) — **CRITICAL DISCOVERY**
- ✅ Schema offsets: C_DOTABaseAbility (10), C_DOTA_Item (3), C_DOTA_UnitInventory (1), +2 поля на C_DOTA_BaseNPC
- ✅ Overlay line 7: game data count + hero ability summary + item count
- ✅ Verified live: 128 heroes, 1898 abilities, 560 items loaded. Abilities + items reading confirmed.

### Критическая находка: два типа network vector

Source 2 использует ДВА layout для `C_NetworkUtlVectorBase<T>`:

1. **Pointer-based** (`CNetworkUtlVectorBase<T>`): `[int32 size][pad 4][T* pData]`
   - Используется: `m_vecAbilities` (21 слот, данные в heap)

2. **Inline** (`CNetworkUtlVectorInline<T>`): `[int32 size][T data[size]]`
   - Используется: `m_hItems` (25 слотов, CHandle[] сразу после count)
   - Обнаружено через pymem hex dump: "pData" = 0xFFFFFFFFFFFFFFFF = два INVALID handle прочитанных как указатель

### Баги найденные и исправленные
- **`%zu` в DEV_LOG** — XorStr + vsnprintf молча съедает вывод, использовать `%d` + `(int)` каст
- **debug.log path** — `GetDllDir()` при manual map = CWD процесса → `реверс курс/debug.log`, НЕ `C:\temp\andromeda\debug.log`

### Файлы (9 изменений)

| Файл | Действие |
|------|----------|
| `Deps/json/json.hpp` | NEW — nlohmann/json v3.11.3 (vendored, 24765 lines) |
| `BotFarm/Data/CGameDataDB.hpp` | NEW — HeroStaticData, AbilityStaticData, ItemStaticData, CGameDataDB |
| `BotFarm/Data/CGameDataDB.cpp` | NEW — JSON loading + hero/ability/item lookups |
| `Dota2/SDK/Types/CEntityData.hpp` | +CNetworkUtlVectorBase, +CNetworkUtlVectorInline, +C_DOTABaseAbility, +C_DOTA_Item, +C_DOTA_UnitInventory, +m_vecAbilities/m_Inventory |
| `BotFarm/GameState/CEntityCache.hpp` | +AbilityData, +ItemData, +FindAbility/FindItem/GetLearnedAbilityCount |
| `BotFarm/GameState/CEntityCache.cpp` | +RefreshAbilities(), +RefreshItems() (inline vector for items) |
| `BotFarm/CBotFarmClient.cpp` | +GameDataDB init, +overlay line 7, fix %zu→%d |
| `BotFarm/Settings/Settings.hpp` | +szDataDir |
| `Andromeda-Dota2-Base.vcxproj` | +CGameDataDB.cpp/.hpp, +json.hpp |

---

## Phase 7: Bot Brain — Lua через sol2 ⭐ (самое жирное)
**Сессий: 3 | Сложность: HIGH | Зависит от: Phases 1, 2, 3, 6**

### SS1+SS2: ✅ DONE (2026-03-28)

**CBotBrain**: sol2 Lua state (Lua 5.4.7 + sol2 v3.3.1), Init/Think/Shutdown lifecycle. C++ fallback brain (retreat/attack/lane). Think 10Hz из OnRender через StateMachine.

**LuaVector**: Vector3 as sol2 usertype — ctor, arithmetic, Length/Normalized/Dot/Cross/Distance.

**LuaConstants**: ~60 constants (BOT_MODE, LANE, UNIT_LIST, DAMAGE_TYPE, ABILITY_BEHAVIOR, etc.)

**LuaUnitProxy**: три sol2 usertypes:
- `UnitHandle(entityIndex)` — ~30 methods, proxy через EntityCache
- `AbilityHandle(entityIndex, slot)` — ~15 methods, GetEntIndex() для PrepareOrders
- `ItemHandle(entityIndex, slot)` — ~8 methods

**CPanoramaJS**: `CUIEngine::RunScript` через `CreateThread`. Game.PrepareUnitOrders для движения/атаки/скиллов. **PrepareUnitOrders из hook threads НЕ работает** — только через CreateThread + RunScript.

**Lua globals**: GetBot(), GetUnitList(), ActionImmediate_*, DotaTime(), GetItemCost(), GetTeam(), print()

**Файлы (12)**: CBotBrain.hpp/cpp, LuaVector.hpp, LuaConstants.hpp, LuaUnitProxy.hpp/cpp, CPanoramaJS.hpp/cpp, Settings.hpp, CEntityCache.hpp/cpp, CBotStateMachine.cpp, CBotFarmClient.cpp, .vcxproj + lua54.lib + Deps/

### SS3: ← NEXT
- Lua script loader (bot_generic.lua, mode_*.lua)
- ~150 stub functions (LUA_STUB_FN macro)
- Lua 5.1→5.4 compat shims
- Full Think loop (mode eval → Think → AbilityUsageThink → LevelUp)
- Hot-reload F9
- C++ fallback brain (keep as safety net)
- Lane functions (GetLaneFrontLocation, LANE_WAYPOINTS)
- Fix: AutoLevel innate, PurchaseItem via Panorama

**Тест**: бот загружает Lua скрипты, идёт в лейн, фармит, кастует. Поведение = Python версия.

---

## Phase 8: ImGui Debug Menu + Config
**Сессий: 1-2 | Сложность: Medium | Зависит от: Phases 1-7**

Табы: Config (аккаунт/герой/лейн), State (стейт-машина), Entities (live list), GC (лог), Brain (mode/desires), Console.

**Тест**: HOME → меню, все табы, конфиг сохраняется.

---

## Phase 9: Multi-Instance
**Сессий: 1-2 | Сложность: Medium | Зависит от: Phases 3, 5**

- Config per DLL из `bot_config.json` рядом с DLL
- ~~`CPanoramaJS`~~ — уже реализован в Phase 7 SS2 (CreateThread + RunScript)
- Python launcher остаётся для multi-instance (AppData redirect, mutex, inject)

**Тест**: 2 инстанса Доты с разными конфигами работают независимо.

---

## Phase 10: VMProtect + Release Build
**Сессий: 1 | Сложность: Low-Medium | Зависит от: все**

VMProtect markers, убрать debug, config encryption, HWID лицензия (опционально).

**Тест**: VMP-обработанная DLL работает.

---

## Граф зависимостей

```
Phase 0 (Scaffold)
  ├── Phase 1 (Entity Cache)
  │     ├── Phase 2 (Game State)
  │     │     └── Phase 3 (Commands) ──┐
  │     └── Phase 6 (Game Data)        │
  │                                     ├── Phase 5 (State Machine)
  └── Phase 4 (GC Messages) ──────────┘        │
                                                ├── Phase 7 (Bot Brain) ⭐
                                                ├── Phase 8 (ImGui Menu)
                                                ├── Phase 9 (Multi-Instance)
                                                └── Phase 10 (VMProtect)
```

---

## Таблица сводка

| Phase | Описание | Сессий | Python источник | Andromeda база |
|-------|----------|--------|----------------|----------------|
| 0 | Scaffold | 1 | — | DllLauncher.cpp |
| 1 | Entity Cache | 1-2 | game_state.py | Hook_OnAddEntity.cpp |
| 2 | Game State | 1 | game_state.py | CFunctionList.hpp |
| 3 | Commands | 1-2 | commands.py (52KB) | — (новый) |
| 4 | GC Messages | 2-3 | gc.py (56KB) | Protobuf/*.pb.h (91!) |
| 5 | State Machine | 2 | test_full_5bot.py | — (новый) |
| 6 | Game Data | 1-2 | game_data.py (12KB) | — (новый) |
| 7 | Bot Brain | **3** | bot_brain.py (163KB) | — (sol2 + Lua) |
| 8 | ImGui Menu | 1-2 | — | CAndromedaGUI.cpp |
| 9 | Multi-Instance | 1-2 | sandbox.py | — (новый) |
| 10 | VMProtect | 1 | — | Config.hpp |
| **Total** | | **15-22** | **~420KB Python** | |

---

## Ключевые преимущества миграции

1. **Shellcode → Direct calls**: VirtualAllocEx + CreateRemoteThread (40 строк) → одна строка function call
2. **Manual protobuf → Classes**: _varint() + _pb_field_* (200 строк) → msg.set_field(value)
3. **RPM entity scan (200ms) → Hook tracking (0ms)**: OnAddEntity/OnRemoveEntity
4. **Hardcoded offsets → Schema resolver**: auto-adaptive к обновам Доты
5. **Python + deps → One DLL**: VMProtect, manual map, easy distribution

---

## Сборка и инжект (текущее состояние)

### Сборка (через batch file + vcvars64)
```bash
# Batch file: C:\temp\build_andromeda.bat
powershell.exe -Command "& 'C:\temp\build_andromeda.bat'" 2>&1 | tail -20
```

Или напрямую (vcxproj, .sln нет в репо):
```bash
# batch содержит: call vcvars64.bat && MSBuild Andromeda-Dota2-Base.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

### Артефакты
```
C:\temp\andromeda_src\Andromeda-Dota2-Base\x64\Release\Andromeda-Dota2-Base.dll  (9.7 MB)
C:\temp\andromeda\Andromeda-Dota2-Base.exe  (572 KB, BlackBone injector)
C:\temp\andromeda\Andromeda-Dota2-Base.dll  (копия для инжектора)
```

**ВАЖНО**: Инжектор (`Andromeda-Dota2-Base.exe`) ищет DLL **рядом с собой** (заменяет .exe→.dll в своём пути). DLL после билда нужно копировать:
```bash
cp /c/temp/andromeda_src/Andromeda-Dota2-Base/x64/Release/Andromeda-Dota2-Base.dll \
   /c/temp/andromeda/Andromeda-Dota2-Base.dll
```

### Инжект (от АДМИНИСТРАТОРА — BlackBone требует PROCESS_ALL_ACCESS)
```bash
# Dota должна быть запущена
powershell.exe -Command "Start-Process 'C:\temp\andromeda\Andromeda-Dota2-Base.exe' -Verb RunAs"
```

**Hot-reload (Phase 3+)**: нажми **END** в игре → DLL "засыпает" (хуки сняты, WndProc восстановлен). Затем заинжекть новую версию — без рестарта Dota. Leak ~2MB за цикл.

### Диагностика
- `C:\temp\andromeda\debug.log` — schema dump + runtime логи
- Консоль: `[+] CBasePattern: <name> -> <addr>` для каждого паттерна
- Config.hpp: `LOG_SDK_PATTERN=1`, `DUMP_SCHEMA_ALL_OFFSET=1`, `ENABLE_CONSOLE_DEBUG=1`
- EntityCache: `grep "\[EntityCache\]" debug.log`
- GameState: `grep "\[GameState\]" debug.log`

---

## Аккаунты (config/accounts.json)

| # | Login | SteamID | Persona |
|---|-------|---------|---------|
| 0 | zrvqd87257 | 76561198725850781 | qt317792 |
| 1 | tqbao71896 | 76561198729640585 | tu483458 |
| 2 | orcby73986 | 76561198727561349 | lc669979 |
| 3 | eearx39260 | 76561198726132021 | zy135930 |
| 4 | bupns61922 | 76561198728730496 | da904387 |

---

## Engine RVA (verified 2026-03-26, all patterns alive)

| Что | Адрес (runtime) | Модуль | Паттерн |
|-----|-----------------|--------|---------|
| GetProtoCDOTAGameAccountPlus | 0x7FF922F6BC40 | client.dll | `48 83 EC ?...` |
| GetLocalPlayerController | 0x7FF922429710 | client.dll | `E8 ? ? ? ? 48 89 44 24...` |
| GetCUserCmdTick | 0x7FF924291400 | client.dll | `48 83 EC ? 4C 8B 0D...` |
| GetCUserCmdArray | 0x7FF9224099D0 | client.dll | `48 89 4C 24 ? 41 56 41 57` |
| GetCUserCmdBySequenceNumber | 0x7FF9224097E0 | client.dll | `40 53 48 83 EC ? 8B DA E8...` |
| SchemaSystem::GetAllTypeScope | 0x7FF9D26978A8 | schemasystem.dll | PTR2 |
| PresentOverlay | 0x7FF9D21C5220 | gameoverlayrenderer64.dll | `48 89 5C 24 ?...` |
| ResizeBuffers | 0x7FF9D21C55F0 | gameoverlayrenderer64.dll | `48 89 5C 24 ?...` |
| CreateSwapChain | 0x7FF9D21C6120 | gameoverlayrenderer64.dll | `48 89 5C 24 08...` |
| OnAddEntity | 0x7FF9224B4E30 | client.dll | `48 89 74 24 ?...FF 81` |
| OnRemoveEntity | 0x7FF9224B5650 | client.dll | `48 89 74 24 ?...FF 89` |
| OnCreateMove | 0x7FF922621040 | client.dll | `85 D2 0F 85...` |
| dota_camera_distance | 0x7FF926DFCCB8 | client.dll | MOV_PTR |
| dota_camera_fog_end | 0x7FF926DFCCC4 | client.dll | MOV_PTR |
| dota_camera_farplane | 0x7FF926DFCCCC | client.dll | MOV_PTR |

### Hardcoded offsets (verify per update):
- `CGameEntitySystem::GetHighestEntityIndex` = 0x20A0
- `CUserCmdArray::m_nSequenceNumber` = 0x5460
- `SchemaSystem::GetClassContainer` = 0x5C0
- VMT: GlobalTypeScope=11, SchemaClassInfo=46, GetLocalPlayer=22

### Python RVA (для сравнения):
- CMDBUF_OFFSET = engine2+0x8CF9E8
- ADDTEXT_RVA = tier0+0x5BC60
- PREPARE_UNIT_ORDERS_RVA = client+0x1E05970
- CUIEngine::RunScript = panorama+0xA6A40
- CUIEngine* singleton = panorama+0x569C78

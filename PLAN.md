# Dota 2 Bot Farm — Implementation Plan

## Context
Автофарм 100 часов Dota 2 на 5-10 аккаунтах. Всё управление через чит (memory R/W). Никакого pixel detection. Боты: пати → поиск → accept → pick → игра → заново.

## Архитектура — 3 слоя

```
┌─────────────────────────────────────────────────────┐
│  ПЕСОЧНИЦА (VM + GPU passthrough)                    │
│  5-10 инстансов Dota 2                              │
└──────────────────────┬──────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────┐
│  ОРКЕСТРАТОР (Python, один процесс)                  │
│  - Координация агентов (asyncio)                    │
│  - Party invite через Steam GC (steamio/dota2)     │
│  - Синхронизация поиска                             │
│  - Рестарт упавших                                  │
└──────────────────────┬──────────────────────────────┘
                       │ (один на инстанс)
┌──────────────────────▼──────────────────────────────┐
│  АГЕНТ = ЧИТ (C++ external или Python pymem)        │
│                                                     │
│  ВСЁ через memory:                                  │
│  - ЧТЕНИЕ: game state, UI state, menu/queue/accept  │
│  - ЗАПИСЬ: console injection → команды движения,    │
│    accept match, pick hero, buy items               │
│  - GSI как бонус (бесплатные данные, zero risk)     │
│                                                     │
│  State machine:                                     │
│  MENU → QUEUE → ACCEPT → HERO_SELECT → IN_GAME →   │
│  POST_GAME → MENU (loop)                            │
└─────────────────────────────────────────────────────┘
```

## Чит — Ядро системы

### Что читаем из памяти

| Данные | Структура | Зачем |
|--------|-----------|-------|
| Game state | CDOTAGameRules::m_nGameState | В меню / в очереди / hero select / в игре / post game |
| Match found | UI state / GC response | Определить что матч найден |
| Hero grid | Entity list + hero class IDs | Для выбора героя |
| Hero position | C_DOTA_BaseNPC_Hero::m_vecOrigin | Координаты нашего героя |
| HP/Mana/Alive | m_iHealth, m_flMana, m_lifeState | Живы ли мы |
| Game time | CDOTAGameRules::m_flGameTime | Сколько длится матч |
| Team | m_iTeamNum | Radiant/Dire |
| Menu/UI state | CDOTA_PanoramaUI или ConVar system | Текущий экран (меню/лобби/матч) |

### Что пишем через memory

**Основной метод: Console Command Injection**

Source 2 позволяет выполнять console commands через `ICvar::DispatchConCommand()`. Находим `ICvar` интерфейс в engine2.dll, вызываем dispatch.

**Команды для каждого шага:**

| Действие | Console Command / Метод |
|----------|------------------------|
| Accept match | `dota_accept_match` / GC message |
| Pick hero | `dota_select_hero npc_dota_hero_luna` (или через GC) |
| Move | `dota_unit_order_move_to_position X Y` / right-click inject |
| Attack | `dota_player_units_auto_attack_mode 1` |
| Buy items | `dota_purchase_item item_tango` |
| Chat wheel | `chatwheel_say N` (для анти-AFK) |

**Альтернатива: Direct Order Injection**

Dota 2 обрабатывает приказы через `CDOTAPlayerController`. Можно напрямую писать в order queue:
```cpp
struct DotaUnitOrder {
    int orderType;      // DOTA_UNIT_ORDER_MOVE_TO_POSITION, ATTACK_TARGET, etc.
    Vector3 position;   // для move
    uint32_t targetIdx; // для attack
    int abilityIdx;     // для cast
};
```

### Определение UI State

**CDOTAGameRules::m_nGameState:**
```
0 = INIT
1 = WAIT_FOR_PLAYERS_TO_LOAD
2 = HERO_SELECTION
3 = STRATEGY_TIME
4 = PRE_GAME
5 = GAME_IN_PROGRESS
6 = POST_GAME
7 = DISCONNECT
```
Если CDOTAGameRules == nullptr → мы в меню.

### Entity System (тот же Source 2 что в CS2)

```
GameResourceServiceClient (engine2.dll) + 0x58 → CGameEntitySystem
CGameEntitySystem → chunks[64] × slots[512]
Каждый слот: CEntityIdentity (stride 0x70)
  +0x10 → entity pointer
  +0x20 → m_designerName ("npc_dota_hero_antimage")
```

### Оффсеты

Дамп через source2gen (ветка dota) или ExistedGit/Dota2Dumper. SchemaSystem идентична CS2.

## GC (Game Coordinator)

Python steamio + dota2 library. Позволяет БЕЗ UI:
- Создавать party, приглашать по Steam ID
- Принимать invite
- Начинать поиск матча
- Принимать матч (accept ready check)
- Выбирать героя

`pip install steam[client] dota2`

## Стек технологий

| Компонент | Технология |
|-----------|-----------|
| Чит (чтение/запись памяти) | Python pymem |
| Console injection | ICvar::DispatchConCommand через memory |
| GC Client (party/accept/pick) | Python steamio + dota2 |
| GSI (бонус, in-game data) | Python aiohttp |
| Оркестратор | Python asyncio |
| Оффсеты | source2gen / Dota2Dumper |

## Структура проекта

```
tools/dota2/
  PLAN.md                  — этот файл
  RESEARCH.md              — результаты ресёрча
  orchestrator.py          — координатор N агентов
  agent.py                 — state machine одного бота
  cheat/
    memory.py              — pymem обёртки (RPM/WPM)
    offsets.py             — оффсеты из dumper
    game_state.py          — чтение CDOTAGameRules, entity list
    commands.py            — console injection, move/attack/buy
  gc/
    gc_client.py           — steamio + dota2 GC wrapper
    party.py               — party/invite/accept/pick
  gsi/
    gsi_server.py          — HTTP listener (бонус)
    gsi_config.cfg         — конфиг для Dota 2
  config/
    accounts.json          — список аккаунтов
    heroes.json            — пул героев для пика
  logs/                    — логи per account
```

---

## Phased Milestones

### Phase 0 — Чит: чтение game state (3-4 часа)
**Статус: ГОТОВО ✓**
- [x] Запустить Dota 2, attach через pymem
- [x] Найти client.dll base, engine2.dll base
- [x] SchemaSystem dumper → offsets.py (+8 shift от source2sdk)
- [x] Читать CDOTAGameRules::m_nGameState → GAME_IN_PROGRESS (5) ✓
- [x] Entity list: 3 chunk lists (ES+0x10/0x18/0x20), 3265 entities в demo
- [x] Читать позицию, HP, team: MK(538hp,Radiant), Rattletrap(692hp,Dire), Doom(648hp,Dire)
- [x] **Milestone: из Python читаем game state и hero position** ✓

**Ключевые находки:**
- Entity System имеет 3 параллельных chunk array (не 1 как в CS2)
- Все schema offsets = source2sdk + 8 байт
- GamerulesProxy→gamerules ptr at +0x5F8 (не 0x510 как в Dota2Patcher)
- designerName для proxy = "dota_gamerules" (не "C_DOTAGamerulesProxy")

### Phase 1 — Чит: отправка команд (3-4 часа)
**Статус: ГОТОВО ✓**
- [x] ConVar direct R/W (ICvar Array3, +0x58=value, +0x60=default)
- [x] CCommandBuffer::AddText shellcode injection (tier0+0x5A4D0)
- [x] Console commands работают: echo, ConVar set, dota_purchase_item
- [x] Hero movement через SendInput right-click (686 units moved)
- [x] **Milestone: из Python двигаем героя** ✓

**Ключевые находки:**
- ConVarData: 0x68 байт, 5226 entries, прямой R/W без code injection
- CCommandBuffer[0] = engine2+0x8C6138, verified +0x8028=0x400
- Console setpos/dota_dev НЕ работают (server-authoritative)
- RVAs стабильны между инстансами (проверено на no-steam)

### Phase 1.5 — SDK + Tooling Setup (2-3 часа)
**Статус: ГОТОВО ✓**
- [x] Скачать SteamDatabase/Protobufs → `tools/dota2/proto/` (78 файлов, unit orders, GC, matchmaking)
- [x] Загрузить client.dll в Ghidra (MCP bridge)
- [x] Найти `PrepareUnitOrders` в client.dll — AOB scan → RVA +0x1D16120
- [x] Верифицировать в Ghidra (FUN_181d16120, 9 params, switch на order types)
- [x] Shellcode injection: move_to(), attack_move(), stop() — всё работает
- [x] **Milestone: герой двигается к координатам без мыши через PrepareUnitOrders** ✓
- [ ] Опционально: neverlosecc/source2sdk (dota branch), dezlock-dump

**Ключевые находки:**
- AOB: `4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08` (19 bytes)
- Local controller: slot=0 at `entity + 0x908`
- 43 order types в protobufs (move, attack, cast, buy, level up...)
- 3/3 integration tests pass (500.0 units moved, 0.0 drift on stop)

### Phase 2 — GC + Multi-Instance
**Статус: В РАБОТЕ**
- [x] Step 1: DotaMemory per-PID + launcher ✓
- [x] Step 2: GC SendMessage + record/replay search ✓
- [x] Step 2A: **Multi-Instance Sandbox** ✓ (2026-03-22)
  - AppData redirect (USERPROFILE/APPDATA/LOCALAPPDATA/TEMP)
  - `-master_ipc_name_override` для IPC изоляции
  - `dota_singleton_mutex` kill через handle64.exe
  - 2 Dota инстанса работают + matchmaking ✓
- [x] Step 2B: **Party invite + accept** ✓ (2026-03-22) — LIVE TESTED
  - `gc.invite_to_party()` + keypress Enter для accept
  - `gc.auto_party()` — полный flow в одном вызове
- [x] Step 2C-queue: **GC Direct Queue** ✓ (2026-03-23)
  - `gc.start_finding_match()` — отправка напрямую через GC, минуя UI
  - **Обходит проверку -tools/-insecure** — GC сервер не знает про launch params
  - Debug tools работают одновременно с matchmaking!
- [ ] Step 2C-accept: Accept match (hook RetrieveMessage → ReadyUpStatus 7170 → CMsgReadyUp 7070)
- [ ] Step 2C-pick: Hero pick (console `dota_select_hero`, ждём game_state == HERO_SELECTION)
- [ ] **Milestone: полный matchmaking flow без UI**

### Phase 3 — Агент: автономный loop (3-4 часа)
**Статус: НЕ НАЧАТ**
- [ ] State machine (7 состояний)
- [ ] Связка: GC (menu flow) + чит (in-game)
- [ ] In-game: walk to lane, auto-attack, die, respawn, repeat
- [ ] Error recovery
- [ ] **Milestone: 1 бот фармит часы 24/7**

### Phase 4 — Масштаб: 5-10 ботов (4-6 часов)
**Статус: НЕ НАЧАТ**
- [ ] Оркестратор: N агентов
- [ ] sandbox.py: автоматический launch_all
- [ ] 5v5 party matchmaking
- [ ] Мониторинг, логирование
- [ ] **Milestone: 10 ботов параллельно**

---

## Песочница — Решение (verified 2026-03-22)

**Метод:** Steam Micro-Copies + AppData Redirect + Registry Switch + Mutex Kill

| Компонент | Решение |
|-----------|---------|
| Steam login isolation | Micro-copies `C:\BotSteam\N\` (junction + real config/, ~2MB) |
| Steam CEF file lock | USERPROFILE/APPDATA/LOCALAPPDATA/TEMP env redirect per instance |
| Steam IPC conflict | `-master_ipc_name_override steamN` |
| Registry AutoLoginUser | `_set_autologin()` перед каждым запуском |
| Game single-instance | Close `dota_singleton_mutex` via handle64.exe |
| Steam auto-update | `Steam.cfg` с `BootStrapperInhibitAll=enable` |
| VAC detection | Ничего не детектится — обычный процесс, env vars, NTFS junctions |

**Ресурсы:** ~1.5GB RAM на инстанс Dota, ~2MB диска на Steam copy (остальное junctions).

### Забракованные варианты

| Вариант | Почему нет |
|---------|------------|
| Копирование Steam | Каждая копия скачивает обновление (~2GB) |
| Отдельные Windows юзеры | 0xc0000142 — проблемы с window station |
| Sandboxie | VAC кикает из matchmaking |
| Docker/WSL2 | Нет GPU |
| `-master_ipc_name_override` без AppData redirect | CEF file lock, GUI не показывается |

## Verification
1. **Phase 0** ✓: Python читает game state
2. **Phase 1** ✓: Move command → герой двигается
3. **Phase 1.5** ✓: PrepareUnitOrders (move без мыши)
4. **Phase 2 (partial)** ✓: GC record/replay search + multi-instance
5. **Phase 2 (full)**: 2 аккаунта → party → queue → accept → pick
6. **Phase 3**: 1 бот: полный цикл menu→game→postgame→menu (3 матча)
7. **Phase 4**: 10 ботов, 24ч непрерывно

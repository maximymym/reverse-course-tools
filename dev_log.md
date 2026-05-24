# Dota 2 Bot Farm — Dev Log

## 2026-03-27 — Phase 5: State Machine (C++ Internal DLL)

### Задача
Автоматический game cycle: inject DLL → auto-queue → accept match → pick hero → play (idle) → disconnect → repeat. Без Python.

### Реализовано: 2 новых файла + 5 edit

**CBotStateMachine** (`BotFarm/StateMachine/CBotStateMachine.hpp/cpp`):
- Enum `EBotState`: 7 состояний (IN_MENU, QUEUING, MATCH_FOUND, HERO_SELECTION, PRE_GAME, GAME_IN_PROGRESS, POST_GAME)
- Singleton + all logic в одном .cpp (~320 строк), без virtual IState
- `OnTick()` в OnRender, rate-limited 100ms (10Hz), `steady_clock`
- First tick: hot inject detection — если gameState >= HERO_SELECTION, прыгает в правильное состояние
- `TransitionTo()`: логирует, вызывает OnEnter, сбрасывает per-state vars
- 7 пар OnEnter/OnTick:
  - **IN_MENU**: autoQueue delay (15s) → QUEUING. Hot-inject detection.
  - **QUEUING**: `StartFindingMatch(region, mode)`, poll GCState.matchFound, timeout 1200s
  - **MATCH_FOUND**: `ReadyUp(lobbyID)`, wait gameState >= HERO_SELECTION, timeout 35s
  - **HERO_SELECTION**: wait ban phase (30s), `dota_select_hero`, retry 5x/2s, verify pHero != nullptr
  - **PRE_GAME**: wait GAME_IN_PROGRESS, timeout 180s
  - **GAME_IN_PROGRESS**: placeholder для Phase 7 BotBrain, check POST_GAME
  - **POST_GAME**: wait 10s, `disconnect`, wait gameState == NONE, timeout 30s

**Settings.hpp** — +13 настроек: bAutoQueue, flAutoQueueDelay(15s), nRegion(0x80), nGameMode(0x02), szHeroName("npc_dota_hero_skeleton_king"), timeouts

**CGCMessageHandler** — +`ResetMatchState()` (clear matchFound/lobbyID/inQueue/queueResult)

**CBotFarmClient.cpp** — SM tick в OnRender, F5/F6 hotkeys, line 6 overlay (orange), Settings include

### Баг: не видит disconnect (PRE_GAME застревает)

Состояния HERO_SELECTION, PRE_GAME, GAME_IN_PROGRESS проверяли только переходы вперёд (gameState >= X), но не disconnect (gameState == NONE/INIT). При ручном disconnect из игры SM застревал.

**Фикс**: добавлена проверка `gameState == NONE || INIT` → TransitionTo(IN_MENU) в трёх OnTick (HERO_SELECTION, PRE_GAME, GAME_IN_PROGRESS).

### Hotkeys (финальный layout)

| Key | Action |
|-----|--------|
| F5 | Toggle auto-queue ON/OFF |
| F6 | Force reset SM → IN_MENU |
| F7 | Manual: start queue (debug, direct GC) |
| F8 | Manual: stop queue (debug) |
| END | Unload DLL |

### Overlay line 6 (orange)
```
SM: IN_MENU | Timer: 5.2s | AutoQ: OFF
```

### Тест (verified live, 2026-03-27)

- ✅ Билд: 0 ошибок
- ✅ Инжект: overlay line 6 отображается
- ✅ F5 toggle: AutoQ ON/OFF
- ✅ Disconnect detection: SM переходит в IN_MENU при disconnect

### Файлы (7 изменений)

| Файл | Действие |
|------|----------|
| `BotFarm/StateMachine/CBotStateMachine.hpp` | NEW — EBotState enum, class declaration |
| `BotFarm/StateMachine/CBotStateMachine.cpp` | NEW — singleton, 7 state handlers, transition table |
| `BotFarm/Settings/Settings.hpp` | +13 settings (autoQueue, matchmaking, hero, timers) |
| `BotFarm/GC/CGCMessageHandler.hpp` | +ResetMatchState() declaration |
| `BotFarm/GC/CGCMessageHandler.cpp` | +ResetMatchState() implementation |
| `BotFarm/CBotFarmClient.cpp` | +SM tick, +F5/F6, +line 6 overlay, +Settings include |
| `Andromeda-Dota2-Base.vcxproj` | +CBotStateMachine.cpp/.hpp |

---

## 2026-03-27 — Phase 4: GC Messages (C++ Internal DLL)

### Задача
Миграция GC (Game Coordinator) из Python gc.py (56KB, shellcode + manual protobuf) на C++ internal DLL с compiled protobuf классами.

### Реализовано: 6 новых файлов + 2 edit

**CGCInterface** (`BotFarm/GC/CGCInterface.hpp/cpp`):
- `SteamInternal_FindOrCreateUserInterface(hUser, "SteamGameCoordinator001")` → ISteamGameCoordinator*
- `SteamAPI_ISteamUser_GetSteamID` → SteamID + AccountID
- `SendGCMessage(msgType, protobuf)` — serialize + 8-byte GC header + vtable[0] SendMessage

**CGCMessages** (`BotFarm/GC/CGCMessages.hpp/cpp`):
- 7 typed senders: InviteToParty, AcceptPartyInvite, LeaveParty, KickFromParty, StartFindingMatch, StopFindingMatch, ReadyUp
- Используют compiled protobuf классы (CMsgStartFindingMatch, CMsgReadyUp, etc.)
- ready_up_key = `lobby_id ^ ~(account_id | account_id << 32)`

**CGCMessageHandler** (`BotFarm/GC/CGCMessageHandler.hpp/cpp`):
- Shellcode VMT hook на ISteamGameCoordinator::RetrieveMessage
- 140-byte x64 shellcode в VirtualAlloc(PAGE_EXECUTE_READWRITE)
- Shared GCHookLog: atomic counter + lastMsgType + lastMsgSize + body copy (up to 0xF00)
- ProcessPending() в OnRender — читает log, парсит protobuf (7034, 7170, 24)
- GCState: partyID, lobbyID, queueResult, inQueue, matchFound, totalMessagesReceived

**CBotFarmClient.cpp** — +GC init, +InstallVMTHook, +ProcessPending в OnRender, +overlay line 5, +F7/F8 hotkeys

### Проблемы и решения (хронология отладки)

1. **MinHook на vtable[2]** — поймал только 5 вызовов при инициализации, потом тишина. Игра перестаёт вызывать RetrieveMessage через vtable после init.
2. **Polling (IsMessageAvailable)** — всегда false. Наш интерфейс подключён к пустой очереди. Сообщения идут во внутренний объект игры.
3. **Polling thread (Sleep 1ms)** — очередь пуста. Даже TIME_CRITICAL тред — пусто, плюс ломает сеть Доты (100% CPU).
4. **ISteamClient::GetISteamGenericInterface vs SteamInternal_FindOrCreateUserInterface** — возвращают РАЗНЫЕ объекты! GetISteamGenericInterface vtable[2] = `0x...D21D4B90`, SteamInternal vtable[2] = `0x...DDEF4B90`. SteamInternal — правильный.
5. **VMT hook (подмена vtable pointer)** — Python gc.py подход. Vtable успешно подменена (verified readback), но 0 вызовов. Причина: **manual-mapped DLL код не исполняется** через vtable calls — Steam SEH молча глотает ACCESS_VIOLATION.
6. **Trampoline в RWX → jmp DLL** — та же проблема (jmp ведёт в non-executable DLL code).
7. **Минимальный shellcode `lock inc; jmp orig`** в RWX — **РАБОТАЕТ!** Counter растёт.
8. **Полный shellcode** (call orig → on success: log msgType/size/body) — **РАБОТАЕТ!** 11+ сообщений, Msgs в overlay растёт.

### Ключевой вывод
**BlackBone manual map НЕ делает code section executable для Steam/Valve внутренних вызовов.** Все vtable hooks из internal DLL должны быть в виде shellcode в VirtualAlloc(RWX). Это отличие от стандартных DLL (LoadLibrary).

### Тест
- ✅ Init: `gc=0x... steamID=76561198725850781`
- ✅ F7: StartFindingMatch → Dota UI "Поиск..."
- ✅ Msgs: N растёт в overlay (receive hook работает)
- ⚠️ F8: StopFindingMatch отправляется но Valve не реагирует (update ~2026-03-25)

---

## 2026-03-26 (вечер) — Первая настоящая 5v5 катка + Controller Fix

### Баг: 2-3 бота AFK (стоят/умирают)
- **Причина найдена**: `_find_local_controller()` при fallback брал **чужой** контроллер (первый попавшийся с valid hero handle)
- В мультибот-сценарии каждый процесс dota2.exe видит ВСЕ контроллеры всех 10 игроков
- Fallback контроллер = чужой игрок → нативный PrepareUnitOrders шёл не тому герою
- При этом Panorama JS fallback НЕ включался, потому что `local_controller != 0` (хоть и чужой)

### Фикс: commands.py `_find_local_controller()`
- Если `hero_entity` задан, но match не найден — ставим `local_controller = 0` вместо fallback
- Это активирует Panorama JS путь (`Game.PrepareUnitOrders()`), который всегда командует локальным игроком конкретного процесса
- Panorama JS fallback подтверждён диагностикой: один бот тикает ~240ms, движется корректно

### Диагностика
- `C:/temp/debug_invite.py` — debug hook на RetrieveMessage, показывает все GC сообщения
- `C:/temp/diag_one_bot.py` — пошаговый tick одного бота с таймингами каждой фазы
- `C:/temp/check_missing.py` — определяет какой инстанс sandbox не запущен по steam_id

### Sandbox: перезапуск упавшего инстанса
- Инстанс #3 (eearx39260) упал — Steam работал, Dota нет
- Нельзя просто `launch_dota()` — старый Steam блокирует. Нужно:
  1. Убить ОБА стима этого инстанса (`powershell Stop-Process -Id X -Force`)
  2. Подождать 2 секунды
  3. `sb.launch_dota(idx)` — запускает Steam+Dota вместе
- Диагностика: `powershell Get-Process steam | Select Id, Path` — показывает какой PID из какого BotSteam

### Полный flow (отработал)
1. **Sandbox**: 5 Steam + 5 Dota, перезапуск упавшего #3
2. **Party invite**: `C:/temp/party_invite.py` — leader invites all, native accept (hook BEFORE invite)
3. **Queue**: replay `config/find_match_template.json` (45B), матч найден мгновенно
4. **Accept**: Panorama JS `DOTAMatchReadyAccept` + GC 7070, 5/5 accepted
5. **Hero pick**: `dota_select_hero` через console, 5 героев (WK, Viper, Sniper, BB, Ogre)
6. **Bot brains**: 5 потоков, think_once() ~240ms/tick, покупка+каст+движение

### Результат первой реальной катки
- Все 5 ботов двигались, покупали предметы, кастовали скиллы
- Качество игры — "дебуст уровень", но все 5 функциональны (не AFK)
- stdout буферизуется в многопоточном режиме — tick-сообщения не видны в реальном времени, но боты работают

### Нерешённые проблемы
- **Sniper (Bot #2) попал на Radiant** вместо Dire (остальные 4 на Dire) — пати split?
- **Bristleback (Bot #3) тоже Radiant** — значит пати не сработало для части, или MM разбросал
- `cache.update()` занимает ~200ms — основной bottleneck (entity scan)
- Нет per-bot логирования в файл (только stdout) — нужно для диагностики в реальных матчах
- Controller matching: 4 из 5 ботов на Panorama JS fallback (только Bot #0 WK matched)

### Файлы изменены
- `cheat/commands.py` — `_find_local_controller()`: не fallback на чужой controller когда hero_entity задан
- `test_full_5bot.py` — Unicode стрелка `→` заменена на `->` (cp1251 fix)
- `C:/temp/party_invite.py` — standalone скрипт party invite
- `C:/temp/pick_heroes.py` — standalone скрипт hero pick
- `C:/temp/run_brains.py` — standalone запуск 5 bot brains
- `C:/temp/start_queue.py` — standalone queue + auto-accept

---

## 2026-03-25 (вечер) — Native Party Accept + Hero Pick Phase Detection

### Native Party Accept (ПРОРЫВ)
- **Проблема**: JS `$.DispatchEvent` и `DispatchPanelEvent` НЕ МОГУТ тригерить нативные C++ хандлеры Panorama (by design)
- GC accept работает (бот вступает в пати), но UI попап "Приглашение не действительно" остаётся навсегда
- `DeleteAsync` на панелях крашит доту. `SetVisible(false)` сбрасывается. SendInput не работает для Panorama.

**Решение**: Frida trace → capstone disasm → shellcode replay
1. Frida hook на `CUIEngine::DispatchEvent` (panorama.dll vtable+0x170) — захватили call stacks при клике Accept
2. Нашли: `panorama.dll → client.dll+0x2b6ddfa (call) → client.dll+0x2daa9b0 (dispatcher) → client.dll+0x2daaa10 (handler)`
3. Capstone disasm `+0x2daa9b0`: switch по event_type, для type=1 → `mov r8,[rdx+0x10]; mov rdx,[rdx+8]; jmp +0x2daaa10`
4. Frida capture args: `rdx = party_id` (точное совпадение!), `r8` не используется sub-handler'ом
5. `rcx = client.dll+0x65599b0` — статический handler object (DOTAAcceptPartyInvite singleton)

**RVAs (client.dll, build 2026-03-25):**
- Handler object: `+0x65599b0` (static global)
- Accept function: `+0x2daaa10` (sub-handler, rcx=handler_obj, rdx=party_id)
- Shellcode: `mov rcx,handler; mov rdx,party_id; sub rsp,0x28; mov rax,func; call rax; add rsp,0x28; ret`

**Результат**: `gc.accept_party_invite_native(party_id)` — принимает инвайт И закрывает попап одним вызовом ✓

### Hero Pick Phase Detection
- **m_nHeroPickState = gamerules + 0x78** — verified via Captain's Draft (values 42→43→44→45)
- 0x80 зеркалит 0x78 (server-side copy)
- All Pick lobby: constant 1 (нет бан-фазы в бот-лобби)
- `phase_hero_pick()` переписан: timing guard 30s + `check_hero_assigned()` verification + fallback heroes

### Panorama Debugging — что НЕ работает (для будущих референсов)
- `$.DispatchEvent("Activated", btn)` — Panorama event, не тригерит нативные handlers
- `$.DispatchEvent("UIPopupButtonClicked", popup)` — не закрывает попап
- `btn.DispatchPanelEvent("onactivate")` — крашит из чужого V8 контекста
- `UiToolkitAPI.CloseAllVisiblePopups()` — не закрывает этот тип попапа
- `popup.DeleteAsync(0)` — крашит доту
- `$.AsyncWebRequest("http://127.0.0.1:...")` — заблокирован Panorama
- `con_logfile` — не работает через наш cmd.execute()
- Единственный способ получить debug output из JS: `$.CreatePanel("Label")` с видимым текстом

### Файлы изменены
- `cheat/offsets.py` — `Gamerules.HERO_PICK_STATE = 0x78`, class `HeroPickPhase`
- `cheat/game_state.py` — `get_hero_pick_state()`, `check_hero_assigned()`, `is_pick_phase()`
- `cheat/gc.py` — `accept_party_invite_native()` (shellcode через CreateRemoteThread)
- `cheat/bot_brain.py` — `GetHeroPickState` мок → реальное чтение из памяти
- `test_full_5bot.py` — `phase_hero_pick()` с timing guard, `_wait_and_accept_with_hook()` → native accept
- `dump_schema_offsets.py` — `m_nHeroPickState` в REQUIRED_FIELDS + probe в `_infer_gamerules()`
- `probe_hero_pick_state.py` — новый файл, standalone probe утилита

---

## 2026-03-26 — 5-Bot Full Flow: Party+Queue+Accept+Pick WORKS, Brain частично

### Что работает
- **Party invite + accept** — все 5 ботов, hook ставится ДО invite (исправлен race condition)
- **Queue** — replay захваченного GC пакета из `config/find_match_template.json`
- **Accept match** — Panorama JS `DOTAMatchReadyAccept` + GC 7070 backup, все 5 accept
- **Hero pick** — `dota_select_hero` через console, все 5 героев
- **Bot brain** — think_once() работает, покупка предметов работает

### Что НЕ работает / проблемы
- **Движение через PrepareUnitOrders** — нативный shellcode не работает, т.к. local_controller не находится
  - Chunk-based entity scan видит только 4 из 5+ контроллеров (слот 0 = наш локальный — отсутствует!)
  - Flat entity list scan тоже не находит (FLAT_STRIDE/FIRST_IDENTITY оффсеты поплыли?)
  - **WORKAROUND**: Panorama JS `Game.PrepareUnitOrders({OrderType:1, Position:[x,y,z]})` — РАБОТАЕТ для всех 5
  - Добавлен `_order_via_panorama()` fallback в `commands.py` — если нет controller, orders идут через JS
- **Рандомно работают 2-3 бота из 5** — остальные AFK
  - Причина не ясна: возможно Panorama fallback не всегда срабатывает, или brain.init() находит чужого героя
  - Нет логов чтобы понять что конкретно происходит в каждом боте

### TODO (следующий промпт)
1. **Полное логирование** — каждый бот должен писать лог в отдельный файл (`logs/bot_0.log`, `logs/bot_1.log`, etc.)
   - Каждый tick: game_state, hero hp/pos, выбранный mode, action, errors
   - controller найден? panorama fallback? order success/fail?
   - hero_name match? entity ptr? _find_our_hero() результат
2. **Диагностика**: запустить 5 ботов с логированием, проанализировать почему 2-3 афк
3. **Починить local_controller** — разобраться почему chunk scan не видит наш слот
   - Возможно EntityIdentity.STRIDE=0x78 поплыл
   - Или часть контроллеров в другом chunk list
   - Или FIRST_IDENTITY offset (0x210) сдвинулся
4. **Стабилизировать Panorama fallback** — убедиться что init_panorama() не фейлит

### Файлы изменены
- `dump_schema_offsets.py` — class descriptor +0x1C→+0x24, +0x28→+0x30
- `cheat/offsets.py` — BaseEntity, BaseNPC, BaseHero, PlayerController, GamerulesProxy, GameSceneNode, BaseAbility
- `cheat/commands.py` — `_find_local_controller()` с hero_entity match, `_order_via_panorama()` fallback, `BOT_HEROES` skeleton_king fix, PLAYER_SLOT 0x908→0x900
- `cheat/bot_brain.py` — controller re-find после hero detection
- `test_full_5bot.py` — полный 6-phase flow (launch→init→party→queue→pick→brain)
- `capture_find_match.py` — захват GC пакета поиска матча из UI
- `config/find_match_template.json` — All Pick + Turbo, EU West + Russia + EU East

---

## 2026-03-25 (поздняя ночь) — Schema Dumper Fix + Full Offset Update

### SchemaSystem class descriptor layout changed
- `member_count` offset: +0x1C → **+0x24**
- `members_base` offset: +0x28 → **+0x30**
- Fixed in `dump_schema_offsets.py`, line 137-138

### Schema dump results (206+107+68 fields found)
- C_DOTA_BaseNPC: 206 fields (was 0 before fix)
- C_DOTAPlayerController: 107 fields
- C_DOTA_BaseNPC_Hero: 68 fields

### BaseEntity offsets (runtime probed, -8 shift)
| Field | Old | New |
|-------|-----|-----|
| m_pGameSceneNode | 0x338 | **0x330** |
| m_iMaxHealth | 0x350 | **0x348** |
| m_iHealth | 0x354 | **0x34C** |
| m_lifeState | 0x358 | **0x350** |
| m_iTeamNum | 0x3F3 | **0x3EB** |

### CGameSceneNode
- VEC_ABS_ORIGIN: 0xD0 → **0xD8** (+8 shift)

### GamerulesProxy
- m_pGameRules: 0x5F8 → **0x5F0** (schema still says 0x5F8, runtime is 0x5F0)

### BaseAbility: +8 shift REMOVED
- Post-update: schema offsets = runtime offsets (no shift needed)
- Verified: m_iLevel=0x628 reads correct level on axe abilities

### BaseNPC (big changes, ~-0xE8 shift for early fields)
| Field | Old | New |
|-------|-----|-----|
| m_iCurrentLevel | 0xC7C | 0xB94 |
| m_flMana | 0xCD4 | 0xBEC |
| m_vecAbilities | 0xD00 | 0xC18 |
| m_Inventory | 0x1168 | 0x1098 |
| m_nUnitState64 | 0x1260 | 0x1190 |

### BaseHero (shifted)
| Field | Old | New |
|-------|-----|-----|
| m_iCurrentXP | 0x1A5C | 0x19A4 |
| m_iPlayerID | 0x1AF8 | 0x1A48 |

### PlayerController (shifted -8)
| Field | Old | New |
|-------|-----|-----|
| m_nPlayerID | 0x908 | 0x900 |
| m_hAssignedHero | 0x90C | 0x904 |

### Engine RVAs — NO CHANGES (all verified)
- CMDBUF_OFFSET, ADDTEXT_RVA, PREPARE_UNIT_ORDERS_RVA — same as prev session
- Panorama RVAs (RunScript, CUIEngine, RunScriptInPanelContext) — unchanged

### Hero Selection — WORKS
- `dota_select_hero` via CCommandBuffer — confirmed working
- **BUG FIX**: Wraith King internal name = `skeleton_king`, NOT `wraith_king`
- Fixed in BOT_HEROES list

### Verified live (GAME_IN_PROGRESS, 4 heroes)
- HP/MaxHP, Team, Position, Level, MoveSpeed, AttackRange, Damage ✓
- Abilities: level, activated, hidden, cooldown ✓
- Hero stats: STR, AGI, INT, XP ✓

### ClientVersion
- cv=6733 (unchanged, VersionDate=Mar 24 2026)

---

## 2026-03-25 (ночь) — Post-Update Fix: CCommandBuffer + ReadyUp cv

### Обнова Dota — все engine RVA сдвинулись

| Константа | Старое | Новое | Модуль | Метод поиска |
|-----------|--------|-------|--------|-------------|
| CMDBUF_OFFSET | 0x8C6138 | **0x8CF9E8** | engine2.dll | Scan .data for verify pattern (+0x8028=0x400), 6 slots stride 0x86D8 |
| ADDTEXT_RVA | 0x5A4D0 | **0x5BC60** | tier0.dll | PE export walk (mangled name) |
| PREPARE_UNIT_ORDERS_RVA | 0x1D162E0 | **0x1E05970** | client.dll | AOB scan (4C 89 4C 24 20 44 89 44 24 18) |
| Panorama RVAs | — | без изменений | panorama.dll | Verified at runtime |

- base_array = engine2 + 0x8CF460 (was 0x8C5BB0)
- AddText найден через `_find_export(pm, "tier0.dll", "?AddText@CCommandBuffer@@...")`
- PrepareUnitOrders: 2 AOB хита, правильный — 0x1E05970 (ближе к старому, использует RIP-relative globals)

### ReadyUp (7070) — нужен client_version!

**Проблема:** `encode_ready_up()` слал только state + ready_up_key. GC молча отклонял — матч отменялся, возврат к поиску.

**Решение:** Добавлен field 3 (client_version varint) в encode_ready_up. cv=6733 (подтверждён через steam.inf и UI capture).

```python
# Было:
body = _pb_field_varint(1, state) + _pb_field_fixed64(2, key)
# Стало:
body = _pb_field_varint(1, state) + _pb_field_fixed64(2, key) + _pb_field_varint(3, cv)
```

### UI Capture: StartFindingMatch (7033) — формат подтверждён

Захвачен реальный пакет из UI через SendMessage hook:
```
f2=4 (matchgroups), f3=6733 (cv), f4=2 (game_modes), f6=0, f7=0,
f10=2, f11="russian" (language), f12=[236B ping_data], f13=3, f14=0,
f16=0, f17=0, f20=0, f22=30, f23=15
```
Наш код работает без f11/f12 — GC принимает. Но UI добавляет язык и ping_data.

### Тест (verified)
```
Party: invite + accept ✓ (cv=6733)
Queue: start_finding_match ✓
Match found: ReadyUpStatus 7170 ✓
Accept: ready_up с cv ✓ (f4 переходит в 9, f6 инкрементируется)
```

### Файлы изменены
```
cheat/commands.py  — CMDBUF_OFFSET, ADDTEXT_RVA, PREPARE_UNIT_ORDERS_RVA
cheat/gc.py        — encode_ready_up +cv, accept_match/decline_match +cv
```

---

## 2026-03-25 (вечер) — GC Party Accept Fix + Post-Update Offsets

### Обнова Dota — все Panorama RVA сдвинулись
- `RunScript`: 0xA8970 → **0xA6A40**
- `CUIEngine ptr`: 0x56D9E8 → **0x569C78**
- `RunScriptInPanelContext`: 0xECEE0 → **0xEAF50**
- `CCommandBuffer offset` тоже сдвинулся (verification fails)
- Найдены через Ghidra: string xref `"RunScriptInPanelContext must be called"` → `FUN_1800eaf50`
- string xref `"CUIEngine::RunScript (compile+run)"` → `FUN_1800a6a40`
- `DAT_180569c78` из деком RunScriptInPanelContext = CUIEngine ptr

### Domain Patch — РАБОТАЕТ
Патч `RunScriptInPanelContext` для bypass domain check:
- AOB: `48 ?? ?? 0F 84 ?? ?? ?? ?? 48 8B 9E 48 02 00 00` (внутри функции, +0x138)
- Патч: CMP+JZ (9B) → NOP*3 + JMP + NOP
- **Важно**: AOB `48 39 F1` (Ghidra) vs `48 3B CE` (реальный бинарник) — обе формы CMP, AOB использует `48 ?? ??`
- **Важно**: AOB НЕ уникален в panorama.dll, сканируем только внутри функции (0x600 байт от RVA)
- Подтверждено: JS выполняется в контексте `PopupPartyInvite#PartyInvitePopup`

### DOTAAcceptPartyInvite — НАТИВНЫЙ HANDLER
**Dispatch через Panorama JS невозможен.** Доказано:
1. Строка `DOTAAcceptPartyInvite` НЕ найдена ни в одном VPK файле (.vjs_c / .vxml_c)
2. `Object.getOwnPropertyNames(this)` в popup context: только стандартные globals + `$`, `panorama`, utility functions
3. `btn.GetAttributeString('onactivate', '')` = пустая строка
4. `$.DispatchEvent('DOTAAcceptPartyInvite', btn)` из popup context — dispatch OK, handler не срабатывает
5. Handler зарегистрирован в C++ через `SetPanelEvent`, не читаем из JS

### Решение: GC-based accept (CMsgPartyInviteResponse 4503)

#### Баг #1: party_id extraction
CacheSubscribed (msg 24) field 3 = **version** (fixed64), НЕ party_id!
Правильно: `f2` (SubscribedType) → inner `f2` (object_data) → inner `f1` (group_id)
Утилита: `extract_group_id_from_cache_subscribed()`

#### Баг #2: client_version
GC молча отклоняет с неправильным cv. После обновы: 6726 → 6733.
Определяется через SendMessage hook на UI invite.

#### Баг #3: ping_data нужен для accept, НЕ нужен для invite
- UI Invite (4501): 20B = steam_id(8B fixed64) + cv(varint) — **без ping_data**
- UI Accept (4503): ~273B = party_id + accept + cv + **ping_data(248B, field 8)**
- Раньше invite слал 241B с ping — GC отклонял

#### Баг #4: start_finding_match (7033) не хватало полей
UI шлёт: f2=matchgroups, **f3=cv**, f4=game_modes, f10=2, f13=3, f22=30, f23=15
Наш код слал только f2 + f4.

### Новые методы в gc.py
| Метод | Описание |
|-------|----------|
| `extract_group_id_from_cache_subscribed(fields)` | Извлекает group_id из SO object |
| `wait_and_accept_invite(timeout)` | Hook → wait CacheSubscribed → extract group_id → send 4503 |
| `auto_detect_cv(save, timeout)` | SendMessage hook, ловит cv из исходящих GC msgs |

### Тест (verified)
```
gc0.invite_to_party(steam_id)           # 20B, cv=6733, no ping
gc1.wait_and_accept_invite(timeout=30)  # hook → group_id → 4503 with cv+ping
# Result: True, party created ✓
```

---

## 2026-03-25 — Panorama JS Execution via CUIEngine::RunScript

### Что сделано
- **Reverse panorama.dll в Ghidra** — полный реверс `CUIEngine::RunScript` (RVA 0xA8970)
- **CUIEngine singleton** — глобальный указатель @ `panorama.dll + 0x56D9E8`
- **CUIEngine layout**: +0x600=V8 Isolate*, +0x5D0=default V8 Context, +0x660=RBTree panel→context
- **Shellcode** — `_build_runscript_shellcode()` → CreateRemoteThread → RunScript(engine, panel, code, origin, flags)
- **Panel-context RBTree** — 17 panel entries, layout A: [left:4][right:4][tag:4][pad:4][key:8][value:8]
- **Навигация к root** — из любого panel через GetParent() loop → `DotaDashboard` (root)
- **FindChildTraverse** — работает: `DashboardPopupManager=FOUND`, `AcceptButton=FOUND` (когда попап активен)
- **`run_panorama_js()`** добавлен в `DotaCommands` — `init_panorama()` + `run_panorama_js(code, panel=0)`
- **`_get_any_panel()`** — читает первый panel из RBTree
- **`_run_dashboard_js()`** — запуск JS в контексте реального panel + навигация к root

### Что работает
- `$.Msg()` — логирование в консоль Dota ✓
- `Game`, `GameUI`, `GameEvents`, `Players`, `Entities`, `Abilities`, `Items` — доступны ✓
- Навигация по UI дереву (FindChildTraverse) ✓
- `$.DispatchEvent('DOTAAcceptPartyInvite', btn)` — выполняется без ошибки ✓

### Что НЕ работает — party invite accept

**Проблема: `$.DispatchEvent('DOTAAcceptPartyInvite', btn)` не вызывает хэндлер.**

Протестировано:
1. **`$.DispatchEvent('DOTAAcceptPartyInvite', btn)`** из контекста каждого из 17 панелей → хэндлер не срабатывает ни из одного. Ивент диспатчится (нет ошибки), но handler попапа не реагирует.
2. **`$.DispatchEvent('Activated', btn)`** → `Event arguments could not be parsed` — нужен аргумент `paneleventsource` (internal-only тип, нельзя передать из JS)
3. **`$.DispatchEvent('Activated', btn, btn)`** → то же: `arguments could not be parsed`
4. **`RunScriptInPanelContext`** на popup/btn → `must be called from a context with a domain that matches this panel` — domain check сравнивает origin URL панелей, наш "code://external" не проходит
5. **`popup.DeleteAsync(0)`** — НЕ тестировался (может просто убрать popup UI)
6. **`$.DispatchEvent('DOTAAcceptPartyInvite')` без panel arg** — dispatch OK, но не работает

### Ключевые находки (Ghidra)

| Функция | RVA | Описание |
|---------|-----|----------|
| CUIEngine::RunScript | 0xA8970 | Compile+Run JS в V8 контексте панели |
| CUIEngine* singleton | 0x56D9E8 (ptr) | Глобальный указатель на движок |
| RunScriptInPanelContext | 0xECEE0 | V8 callback, domain check (code:// vs code://) |
| CreatePanoramaUIEngineInternal | 0x97150 | Фабрика CUIEngine |
| panorama_dispatch_event handler | 0xAFB70 | Console cmd: CreateEventFromString(engine, &out, **0**, str) — NULL panel! |
| CreateEventFromString | vtable+0x328 | Парсит строку → event object |
| DispatchEvent | vtable+0x170 | Диспатч event object |
| Activated event registration | 0x4AFA0 | RegisterEvent("Activated", 1 arg: paneleventsource) |

### Архитектурные заметки
- **Default V8 context** (panel=NULL): имеет `$`, `Game`, `GameUI` и т.д., но `$.GetContextPanel() = NULL`
- **Panel V8 contexts** (17 штук в RBTree): каждый имеет `$.GetContextPanel()` с реальным panel
- **Popup V8 context**: НЕ создаёт отдельный контекст в RBTree (0 новых после инвайта). Popup переиспользует существующий контекст одной из 17 панелей
- **RunScriptInPanelContext domain check**: сравнивает `panel->vtable[0x158/8]()` (origin URL) вызывающей и целевой панели. Обе должны начинаться с `"code://"`. Наш origin `"code://external"` не помогает — проверяется origin ПАНЕЛИ, не скрипта
- **Activated event**: internal-only, аргумент `paneleventsource` не парсится из V8 JS объектов
- **DOTAAcceptPartyInvite**: зарегистрирован в client.dll (не panorama.dll), хэндлер привязан к V8 контексту попапа

### Файлы изменены
- `tools/dota2/cheat/commands.py` — +200 строк: Panorama JS execution, RBTree reader, dashboard JS helpers

## 2026-03-24 — Match Accept Fix: ready_up_key XOR

### Проблема
`accept_match()` отправлял `lobby_id` напрямую как `ready_up_key` в `CMsgReadyUp` → GC молча отклонял. Из SteamKit issue #297: `ready_up_key` — это **не** сырой `lobby_id`.

### Формула (SteamKit #297)
```python
ready_up_key = lobby_id ^ (~(account_id | (account_id << 32)) & 0xFFFFFFFFFFFFFFFF)
```

### Авто-определение SteamID
Для XOR нужен `account_id` (нижние 32 бита SteamID64). Реализован авто-резолв через шеллкод:
1. `SteamAPI_SteamUser_v023()` → `ISteamUser*`
2. `SteamAPI_ISteamUser_GetSteamID(ISteamUser*)` → `CSteamID` (uint64 в RAX)

Оба — flat C API экспорты из `steam_api64.dll` (индексы 1008, 857). Vtable-подход (`GetISteamGenericInterface("SteamUser021")`) **не работал** — `SteamUser` не generic interface.

Экспорт `SteamUser` (без префикса) тоже **не существует** в steam_api64.dll Dota 2.

### Изменения в `cheat/gc.py`
| Что | Описание |
|-----|----------|
| `compute_ready_up_key(lobby_id, account_id)` | Новая функция — XOR формула |
| `DotaGC.__init__(mem, steam_id=0)` | +`steam_id`, +`account_id` |
| `DotaGC._resolve_steam_id()` | Шеллкод: flat C API (SteamAPI_SteamUser_v023 + SteamAPI_ISteamUser_GetSteamID) |
| `DotaGC.init()` | Авто-вызов `_resolve_steam_id()` если `steam_id == 0` |
| `accept_match(lobby_id)` | Параметр переименован, XOR key вычисляется внутри |
| `decline_match(lobby_id)` | Аналогично |

### Тест
```
[+] SteamID: 76561198725850781 (account_id: 765585053)
[!!!] MATCH FOUND! lobby_id=0x69bb460c46c7b3
[>] AcceptMatch lobby=0x69bb460c46c7b3 key=0xd237a224de18ded1 account_id=765585053
[+] Accept sent (panorama + GC)
```
Матч принят успешно (Panorama + GC backup).

### steam_api64.dll экспорты (справка)
- `SteamAPI_SteamUser_v023` — accessor, возвращает `ISteamUser*`
- `SteamAPI_ISteamUser_GetSteamID` — flat C wrapper для vtable[2]
- `SteamClient` — возвращает `ISteamClient*` (используется для GC)
- `SteamAPI_GetHSteamUser` / `SteamAPI_GetHSteamPipe` — handles
- Всего 1046 экспортов

## 2026-03-22 — Phase 0: Game State Reading

### Attach + Modules
- pymem attach → PID 30948
- `client.dll`: 0x7FFA3B7F0000 (size ~108MB)
- `engine2.dll`: 0x7FFA66810000
- `schemasystem.dll`: 0x7FFAD2FB0000

### Interface Resolution
- CreateInterface pattern: `4C 8B ?? ?? ?? ?? ?? 4C 8B ?? 4C 8B ?? 4D 85 ?? 74 ?? 49 8B ?? ?? 4D 8B`
- Работает: ходим по linked list (node: +0x0=fn_ptr, +0x8=name_ptr, +0x10=next)
- `fn_ptr` → LEA rax, [rip+X] → absolute_address(fn_ptr, 3, 7)
- **GameResourceServiceClientV001**: resolved → 0x7FFA66E20CF0
- **SchemaSystem_001**: resolved → 0x7FFAD3026630

### Entity System
- `GameResourceService + 0x58` → CGameEntitySystem = 0x405C7383800
- **Chunk-based access** (не linked list!):
  - `entity_system + 0x10` → chunk_list pointer
  - 64 chunks × 512 slots, stride 0x78 (CEntityIdentity)
  - 547 entities в меню, 116 уникальных типов
- **Linked list** (`+0x210 → m_pNext at +0x60`): только ~4 active entities
- **Entity pointer**: `identity + 0x00` = CBaseEntity* (read_ptr, NOT identity-0x10!)
- **designerName**: `identity + 0x20` → CUtlSymbolLarge → char*

### SchemaSystem Dumper
- `SchemaSystem + 0x198` → scopes list (NOT 0x190 as in Dota2Patcher)
- 20 scopes: schemasystem.dll, engine2.dll, client.dll, server.dll, etc.
- Containers at scope + 0x580, stride 0x28, ptr at +0x18
- ClassDescription: +0x8=name_ptr, +0x18=size, +0x1C=members_count, +0x28=members_base
- Field: +0x0=name_ptr, +0x8=type_ptr, +0x10=offset, +0x14=netvar_flag
- **Dumped**: 2726 classes, 52607 netvars (only leaf classes — base classes via parent chain)
- Saved to `offsets_dump.json`

### Offset Discovery — +8 Byte Shift
- source2sdk (Aug 2025 dota branch) offsets are ALL shifted by +8 in current version
- Verified empirically on monkey_king entity:

| Field | source2sdk | Actual | Verified |
|-------|-----------|--------|----------|
| m_pGameSceneNode | 0x330 | **0x338** | ptr → pos (-2456, -2176, 128) ✓ |
| m_iHealth | 0x34C | **0x354** | 538 ✓ |
| m_lifeState | 0x350 | **0x358** | 0 (alive) ✓ |
| m_iTeamNum | 0x3EB | **0x3F3** | 2 (Radiant) ✓ |
| m_iszUnitName | 0xD40 | **0xD48** | "npc_dota_hero_monkey_king" ✓ |
| m_iDamageMin | 0xDE8 | **0xDF0** | 53 ✓ |

- CGameSceneNode::m_vecAbsOrigin = 0xD0 (NOT shifted — separate class)

### GameRules
- C_DOTAGamerulesProxy NOT found in menu (expected — only exists in-game)
- When found: proxy + 0x518 → CDOTAGamerules (was 0x510, +8 shift)
- Gamerules offsets from Dota2Patcher (direct, no shift): game_state=0x74, etc.
- **TODO**: verify in actual match

### Key References
- **neverlosecc/source2sdk** (dota branch, Aug 2025) — class layouts
- **Wolf49406/Dota2Patcher** — runtime patterns, entity system, SchemaSystem walker
- **ikhsanprasetyo/dota2dumped** — interface offsets (Feb 2024, outdated)
- **tranqu1lizer/dota_new_hack** — C++ internal hack reference

### Files Created
```
cheat/
  __init__.py
  memory.py      — pymem wrappers (RPM, pattern scan, interface walking)
  offsets.py     — all offsets (verified +8 shift)
  game_state.py  — DotaGame class (init, entities, hero data)
test_phase0.py   — integration test
dump_offsets.py  — SchemaSystem runtime dumper
offsets_dump.json — full 52607 netvar dump
```

### Entity System — 3 Chunk Lists (CRITICAL FINDING)
- Entity system at GRS+0x58 has **3 parallel chunk arrays**:
  - ES+0x10: main entities (gamerules, player_controllers, towers, heroes)
  - ES+0x18: hero duplicates, cosmetics
  - ES+0x20: in-game entities (heroes, fountain, abilities)
- Each chunk list: 64 chunks × 512 slots × stride 0x78
- Same entity can appear in multiple lists (different identity addresses, same entity ptr)
- **Must scan all 3 lists** for complete coverage

### GameRulesProxy — Correct Offset
- designerName = `"dota_gamerules"` (NOT `"C_DOTAGamerulesProxy"`)
- binary_name (SCB+0x38) = `"C_DOTAGamerulesProxy"` (confirmed)
- **proxy + 0x5F8** → CDOTAGamerules* (NOT 0x510, NOT 0x518, NOT 0x5E8)
- GameRules verified in demo mode:
  - game_state@+0x74 = 5 (GAME_IN_PROGRESS) ✓
  - game_state@+0x7C = 5 ✓
  - game_mode@+0xE4 = 15 (demo)
  - paused@+0x38 = 0

### SchemaClassBinding Layout (differs from Dota2Patcher)
- SCB+0x00 → SchemaName → class_name (e.g. "C_DOTA_BaseNPC_Hero")
- SCB+0x38 → SchemaName → binary_name (e.g. "C_DOTA_Unit_Hero_MonkeyKing")
- SCB+0x40 → SchemaName → parent class name
- SchemaName+0x08 → char* (name string)

### In-Game Test Results (demo mode, 2026-03-22)
- 3265 entities, 892 unique types
- Heroes found: monkey_king (Radiant, HP=538), rattletrap (Dire, HP=692), doom_bringer (Dire, HP=648)
- Positions valid, teams correct (2=Radiant, 3=Dire)
- Level=-1 for some heroes (demo mode quirk?)
- Mana=0 for all (demo mode doesn't set mana?)

### Phase 0 — COMPLETE ✓

---

## 2026-03-22 — Phase 1: Command Injection

### ICvar / ConVar System (tier0.dll)
- `VEngineCvar007` interface found in tier0.dll (RVA 0x3A33F0)
- ICvar object stores ConVars in Array3: indexed entries at ICvar+0x48, count at ICvar+0x40 (lo16)
- Array3 format: pairs of (ConVarData_ptr, index) at stride 0x10
- **5226 ConVars** in current build

### ConVar Data Layout (0x68 bytes per entry)
```
+0x00: char* name
+0x08: void* (self-ptr to +0x60)
+0x20: char* description
+0x28: type/bounds info (lo=max_val, hi=has_bounds)
+0x30: uint32 FCVAR_* flags
+0x58: CVValue_t current_value (i32/f32 union, 4 bytes)
+0x60: CVValue_t default_value
```
- Direct R/W verified: sv_cheats=1 (demo mode), developer=0, auto_attack_mode=1
- Float convars (dota_camera_distance=1200.0, sensitivity=1.25) also work via f32 read/write

### CCommandBuffer::AddText (tier0.dll)
- Export: `?AddText@CCommandBuffer@@QEAA_NPEBDHH_NN_K@Z` at tier0+0x5A4D0
- Signature: `bool AddText(const char* cmd, int delay, int unk, bool unk, bool unk, uint64_t flags)`
- IAT entry in engine2.dll at engine2+0x47EC88
- **4 callers** found in engine2: +0x79E31, +0x1C0629, +0x1CDD0F, +0x1CEBE3

### CCommandBuffer Instance (engine2.dll)
- Caller analysis: `this = base + slot * 0x86D8 + 0x588`
- base_array = engine2 + 0x8C5BB0 (verified via LEA rcx in multiple callers)
- **CCommandBuffer[slot=0] = engine2 + 0x8C6138** (client command slot)
- Verification: +0x8028 = 0x400 (max_arg_buffer_size) ✓
- Object size ~0x8150 bytes (fields at +0x8000..+0x8068)

### Shellcode Injection
- x64 shellcode: sub rsp → mov rcx(this) → mov rdx(cmd) → xor r8/r9 → zero stack args → call AddText → ret
- VirtualAllocEx + WriteProcessMemory + CreateRemoteThread
- **CRITICAL**: ctypes argtypes MUST be set for 64-bit (c_ulonglong for addresses), otherwise truncated to 32-bit
- Verified: `echo` → appears in console, `dota_player_units_auto_attack_mode 3` → ConVar changes ✓

### Hero Movement
- Console commands (setpos, dota_dev, ent_fire) — **НЕ РАБОТАЮТ** в Dota 2 (server-authoritative)
- +forward — двигает камеру, не героя
- **SendInput right-click** — РАБОТАЕТ! Герой двигается к точке клика
- Test: Troll Warlord (-2736,1193) → (-2061,1314) = **686 units** за 2.5с ✓
- Position sampling каждые 0.5с подтверждает плавное движение

### RVA Summary (stable across instances)
| Что | RVA | Модуль |
|-----|-----|--------|
| GameResourceService | +0x610CF0 | engine2.dll |
| EntitySystem | GRS+0x58 | — |
| ICvar | +0x3A33F0 | tier0.dll |
| CCommandBuffer[0] | +0x8C6138 | engine2.dll |
| AddText | +0x5A4D0 | tier0.dll |
| AddText IAT | +0x47EC88 | engine2.dll |

### Files Created/Modified
```
cheat/
  commands.py    — DotaCommands (shellcode cmd injection) + ConVarSystem (direct R/W)
test_phase1.py   — integration test (4/5 pass: echo, convar_direct, convar_cmd, purchase)
test_movement.py — mouse movement test (686 units moved ✓)
research_phase1*.py — research scripts (a-k), можно удалить
```

### Phase 1 — COMPLETE ✓

### Ресёрч: инструменты для ускорения

Ручной реверс каждого оффсета — слишком медленно. Найденные ресурсы:

1. **SteamDatabase/Protobufs** (github) — ВСЕ protobuf определения Dota 2:
   - `CDOTAMsg_UnitOrder` — 42 типа приказов (MOVE_TO_POSITION=1, ATTACK_TARGET=4, PURCHASE_ITEM=16...)
   - `CDOTAClientMsg_ExecuteOrders` — обёртка для отправки (msg_id=350)
   - `CMsgReadyUp`, `CMsgStartFindingMatch` — весь matchmaking flow
   - Обновляется автоматически с каждым патчем Valve

2. **neverlosecc/source2sdk** (dota branch) — полный C++ SDK, 1000+ headers, все классы

3. **ExistedGit/Dota2Cheat** — reference: `PrepareUnitOrders` (movement без мыши) + protobuf integration

4. **dezlock-dump** — 23,000+ классов включая internal (не только schema), vtable RVAs, RTTI

5. **PrepareUnitOrders** — ключевая функция для движения:
   ```
   void PrepareUnitOrders(CDOTAPlayerController*, CDotaUnitOrder orderType,
       int entityHandle, Vector* pos, int abilityIdx,
       PlayerOrderIssuer_t issuer, CBaseEntity* unit, OrderQueueBehavior_t, bool showEffect)
   ```

6. **Panorama JS** — `Game.PrepareUnitOrders()` — нативный JS API, zero reverse engineering

---

## 2026-03-22 — Phase 1.5: PrepareUnitOrders + Protobufs

### PrepareUnitOrders — Found & Working!

**AOB Scan** (19 bytes, from soyware/heck_dota):
```
4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08
```
This is the function prologue saving register params to shadow space:
- `mov [rsp+20h], r9` (position ptr)
- `mov [rsp+18h], r8d` (targetIndex)
- `mov [rsp+10h], edx` (orderType)
- `mov [rsp+08h], rcx` (this)

**Result**: RVA `+0x1D16120` in client.dll, verified in Ghidra at `0x181D16120` (imagebase `0x180000000`).

**Prototype (x64 __fastcall)**:
```cpp
void PrepareUnitOrders(
    CDOTAPlayerController* this,  // RCX
    DotaUnitOrder_t orderType,    // EDX (1=move, 3=a-move, 4=attack, 21=stop, ...)
    int targetIndex,              // R8D (entity index for target orders)
    Vector3* position,            // R9 (world pos for move/cast)
    int abilityIndex,             // [RSP+20h]
    PlayerOrderIssuer_t issuer,   // [RSP+28h] (2=HERO_ONLY)
    CBaseEntity* unit,            // [RSP+30h] (0=auto)
    OrderQueueBehavior_t queue,   // [RSP+38h] (0=default)
    bool showEffects              // [RSP+40h]
);
```

### Local Player Controller
- 4 `dota_player_controller` entities in demo mode
- Local player = **slot 0** (`controller + 0x908 == 0`)
- Player name at `controller + 0x6E0` (inline string)
- Entity index 1 (handle 0x1110001)

### String References Found
- `"PrepareUnitOrders"` at RVA `+0x4A63750` (Panorama JS binding registration)
- `"particles/ui_mouseactions/waypoint_flag.vpcf"` at RVA `+0x3FAFAD0` (xref → function)
- `"courier_go_to_secretshop"` at RVA `+0x400B148`

### Shellcode — 9-param x64 call
Same pattern as AddText shellcode: VirtualAllocEx → write shellcode + Vector3 → CreateRemoteThread.
```
sub rsp, 0x58
mov rcx, <controller>      ; this
mov edx, <orderType>       ; e.g. 1 for MOVE
mov r8d, <targetIndex>
mov r9, <vec3_ptr>
mov [rsp+20h], <ability>
mov [rsp+28h], <issuer>    ; 2 = HERO_ONLY
mov [rsp+30h], <unit>
mov [rsp+38h], <queue>
mov byte [rsp+40h], <fx>
mov rax, <PrepareUnitOrders>
call rax
add rsp, 0x58
xor eax, eax
ret
```

### Movement Test Results
```
MOVE_TO_POSITION: hero moved exactly 500.0 units in 2.0s ✓
STOP: hero stopped instantly, 0.0 drift ✓
ATTACK_MOVE: hero a-moved 386.6 units (stopped to attack creep) ✓
```
**3/3 integration tests passed.**

### Protobufs Downloaded
- 78 .proto files from SteamDatabase/Protobufs → `tools/dota2/proto/`
- Key files:
  - `dota_commonmessages.proto` — `dotaunitorder_t` enum (43 values), `CDOTAMsg_UnitOrder`
  - `dota_clientmessages.proto` — `CDOTAClientMsg_ExecuteOrders`
  - `dota_gcmessages_client_match_management.proto` — `CMsgStartFindingMatch`, `CMsgReadyUp`

### Unit Order Types (from protobufs)
| Value | Name | Use case |
|-------|------|----------|
| 1 | MOVE_TO_POSITION | Walk to XYZ |
| 3 | ATTACK_MOVE | A-click |
| 4 | ATTACK_TARGET | Attack entity |
| 5 | CAST_POSITION | Ground-targeted ability |
| 6 | CAST_TARGET | Unit-targeted ability |
| 8 | CAST_NO_TARGET | Toggle/passive ability |
| 10 | HOLD_POSITION | Hold |
| 11 | TRAIN_ABILITY | Level up |
| 16 | PURCHASE_ITEM | Buy from shop |
| 21 | STOP | Stop all |
| 23 | BUYBACK | Buyback |

### Files Created/Modified
```
cheat/
  commands.py    — +PrepareUnitOrders shellcode, order(), move_to(), attack_move(), stop(), etc.
  offsets.py     — +PREPARE_UNIT_ORDERS_RVA, PlayerController class
proto/           — 78 protobuf files (SteamDatabase/Protobufs dota2/)
test_phase15.py  — 3 integration tests (move_to, stop, attack_move) — all pass
research_phase15_a.py — AOB scan + string search
research_phase15_b.py — controller field analysis
research_phase15_c.py — movement proof of concept
```

### RVA Summary (updated)
| Что | RVA | Модуль |
|-----|-----|--------|
| GameResourceService | +0x610CF0 | engine2.dll |
| ICvar | +0x3A33F0 | tier0.dll |
| CCommandBuffer[0] | +0x8C6138 | engine2.dll |
| AddText | +0x5A4D0 | tier0.dll |
| **PrepareUnitOrders** | **+0x1D16120** | **client.dll** |

### Phase 1.5 — COMPLETE ✓

---

## 2026-03-22 — Phase 2: Multi-Instance + GC

### Step 1: DotaMemory per-PID + Launcher
- `DotaMemory(pid=N)` — attach to specific PID (multi-instance support)
- `launcher.py` — process enumeration via Win32 API (CreateToolhelp32Snapshot)
- `DotaLauncher` — reads `config/accounts.json`, launches Steam copies with `-master_ipc_name_override`
- **Verified**: PID discovery finds running dota2.exe, per-PID attach works ✓

### Step 2: ISteamGameCoordinator — GC Message Sender

**Finding the GC interface:**
1. SteamAPI exports in steam_api64.dll:
   - `SteamClient()` at RVA +0x8DD0
   - `SteamAPI_GetHSteamUser()` at RVA +0x7A90
   - `SteamAPI_GetHSteamPipe()` at RVA +0x7A80
2. Shellcode: SteamClient() → ISteamClient::GetISteamGenericInterface("SteamGameCoordinator001")
   - vtable[12] (+0x60) = GetISteamGenericInterface
3. **ISteamGameCoordinator* = 0x2236C441830** (runtime)
4. VMT in steamclient64.dll:
   - vtable[0] = SendMessage
   - vtable[1] = IsMessageAvailable
   - vtable[2] = RetrieveMessage

**Ghidra analysis:**
- GC init function: FUN_18378DA20 (client.dll RVA +0x1F6DA20)
- CGCClientSystem stores ISteamGameCoordinator* at (this + 0xB8 + 0x18)
- xref to "SteamGameCoordinator001" string at client.dll RVA +0x4FCBD50

**GC message format:**
```
SendMessage(msgType | 0x80000000, data, size)
data = [header_size: uint32 LE][CMsgProtoBufHeader: 0 bytes][body: protobuf]
```

**Key GC messages (from SteamDatabase/Protobufs):**
| msg_type | Name | Proto fields |
|----------|------|-------------|
| 7033 | CMsgStartFindingMatch | matchgroups(f2), game_modes(f4) |
| 7036 | CMsgStopFindingMatch | accept_cooldown(f1) |
| 7070 | CMsgReadyUp | state(f1), ready_up_key(f2 fixed64) |
| 7170 | CMsgReadyUpStatus | lobby_id, accepted/declined |

**Game mode bitmasks (from auto-search.lua):**
- All Pick = 1<<1, Turbo = 1<<23, Single Draft = 1<<4

**Region bitmasks:**
- Russia = 1<<7, EU West = 1<<2, EU East = 1<<8

**Manual protobuf encoding** — no external library needed, just varint + field tags.

**Test result:** `gc.start_finding_match(TURBO, RUSSIA)` → SendMessage executed, no crash ✓
- TODO: verify UI shows "Searching" (need user confirmation)

### GC Message Recording + Replay ✓

**VMT Hook на ISteamGameCoordinator::SendMessage:**
1. Читаем оригинальный vtable из `[gc_ptr]`
2. Аллоцируем новый vtable + hook shellcode + log buffer (4KB)
3. Hook: сохраняет msg_type + size + raw data в log buffer → вызывает оригинал
4. Подменяем `[gc_ptr]` на новый vtable
5. Ждём msg_type 7033 (CMsgStartFindingMatch)
6. Восстанавливаем оригинальный vtable

**Результат:** перехвачено 294 байт реального CMsgStartFindingMatch — включает ping_data, client_version, языки, регионы — всё что нужно для валидного поиска.

**Replay:** `gc.replay_search()` отправляет записанное сообщение через `send_raw_gc_message` → UI показывает "Searching for match" ✓

**API:**
```python
gc.record_search(timeout=60)  # юзер кликает Find Match → записывается
gc.replay_search()             # повторяет записанный поиск
gc.stop_finding_match()        # отмена
```

### Files Created/Modified
```
cheat/
  memory.py     — +pid parameter in DotaMemory constructor
  gc.py         — NEW: DotaGC (resolve + SendMessage + record/replay + protobuf)
                  Methods: init, send_gc_message, send_raw_gc_message,
                  record_search, replay_search, start_finding_match,
                  stop_finding_match, accept_match, decline_match
launcher.py     — NEW: DotaLauncher (multi-instance, process enum)
config/
  accounts.json    — instance config template
  last_search.json — recorded CMsgStartFindingMatch (294B, auto-generated)
test_gc.py         — GC init + matchmaking test
research_phase2_gc*.py — research scripts (3 files)
```

### Phase 2 Status
- Step 1 ✓: DotaMemory per-PID + launcher
- Step 2 ✓: GC SendMessage + record/replay search
- Step 3: Accept match + pick hero — NEXT
- Step 4: Orchestrator + state machine

---

## 2026-03-22 — Phase 2A: Multi-Instance (Sandbox)

### Problem: Running Multiple Dota 2 on One PC

**Three obstacles solved:**

1. **Steam CEF file locking** — modern Steam stores Chromium (CEF) cache in AppData with exclusive locks. Second Steam instance can't show GUI.
2. **Steam IPC collision** — multiple Steam processes interfere via shared IPC.
3. **dota_singleton_mutex** — Source 2 engine creates a named mutex to prevent multiple game instances.

### Failed Approaches

| Approach | Result | Why |
|----------|--------|-----|
| `-master_ipc_name_override` alone | No GUI on 2nd instance | CEF cache locked in shared AppData |
| Copy Steam to separate dirs | Updates re-download (~2GB) | Different Steam install = full update |
| Separate Windows users + CreateProcessWithLogonW | 0xc0000142 (DLL init fail) | Window station permissions |
| Sandboxie | VAC kicks from matchmaking | Anti-cheat detects sandbox |

### Working Solution: AppData Redirect + Mutex Kill

**Method:** Override `USERPROFILE`, `APPDATA`, `LOCALAPPDATA`, `TEMP`, `TMP` environment variables per instance, giving each Steam its own profile directory. Combined with `-master_ipc_name_override` for IPC isolation. After first Dota launches, close `dota_singleton_mutex` via Sysinternals handle64.exe to allow second+ instances.

**Why VAC can't detect:** No hooks, no DLL injection, no sandbox. Just standard environment variables and a legitimate process. The mutex is closed from outside (handle64.exe), not patched in memory.

**Profile structure:**
```
C:\BotProfiles\
  bot0\
    AppData\Roaming\    — Steam CEF cache, config
    AppData\Local\      — Steam local data
    Temp\               — temp files
  bot1\
    AppData\Roaming\
    AppData\Local\
    Temp\
```

**Launch sequence:**
```
1. Steam #0: env USERPROFILE=C:\BotProfiles\bot0 ... steam.exe -master_ipc_name_override steam0
2. Steam #1: env USERPROFILE=C:\BotProfiles\bot1 ... steam.exe -master_ipc_name_override steam1
3. Log in to both Steam instances (manually, first time only)
4. Dota #0: launch via Steam #0 -applaunch 570
5. close_dota_mutex() — kills dota_singleton_mutex on Dota #0
6. Dota #1: launch via Steam #1 -applaunch 570
```

### Steam Update Prevention

Created `C:\Program Files (x86)\Steam\Steam.cfg`:
```
BootStrapperInhibitAll=enable
BootStrapperForceSelfUpdate=disable
```
Delete this file when you want to update Steam manually.

### Mutex Details

- **Name:** `dota_singleton_mutex`
- **Type:** Mutant (Windows kernel named mutex)
- **Location:** `\Sessions\1\BaseNamedObjects\dota_singleton_mutex`
- **Tool:** `C:\temp\handle64.exe` (Sysinternals Handle v5.0)
- **Command:** `handle64.exe -accepteula -c <handle_hex> -p <dota_pid> -y`

### Verified ✓

- [x] Two Steam instances with separate GUI
- [x] Login works on both instances (different accounts)
- [x] Two Dota 2 instances running simultaneously
- [x] Matchmaking works — both instances can queue and find lobby

### Files Created/Modified
```
sandbox.py          — NEW: BotSandbox class (AppData redirect + mutex kill)
                      Methods: setup, launch_steam, launch_dota, launch_all_steam,
                      launch_all_dota, close_dota_mutex, status, kill_all, cleanup
config/sandbox.json — NEW: sandbox configuration (count, paths)
C:\BotProfiles\     — NEW: per-instance profile directories
C:\Program Files (x86)\Steam\Steam.cfg — NEW: update prevention
C:\temp\handle64.exe — Sysinternals Handle (downloaded)
Desktop\steam_multi.bat — bat launcher (legacy, use sandbox.py instead)
Desktop\launch_dota.bat — bat launcher (legacy)
setup_multi.py      — DEPRECATED (was copying Steam, not needed)
launcher.py         — updated (new config format, launch_steam/launch_dota)
```

### Registry AutoLogin Fix

**Проблема:** Steam читает `HKCU\Software\Valve\Steam\AutoLoginUser` для автологина. Реестр общий — второй аккаунт перезаписывал первый.

**Решение:** `_set_autologin(idx)` — перед запуском парсит `loginusers.vdf` из `BotSteam/N/config/`, ставит правильный `AutoLoginUser` + `SteamPath` в реестр.

### Minimal Steam Copies (Junctions)

**Проблема:** Оба инстанса шарили `Steam\config\` — один логин на всех.

**Решение:** `C:\BotSteam\0\` и `C:\BotSteam\1\` — micro-copies (~2MB каждая):
- `config/` — REAL (per-instance, loginusers.vdf)
- Всё остальное — NTFS junction на оригинальный Steam
- `Steam.cfg` — блокировка обновлений

### Final Launch Sequence (verified)

```
1. sb.launch_steam(0, with_dota=True)
   → registry AutoLoginUser = account_0
   → steam.exe from BotSteam/0 + env redirect BotProfiles/bot0
   → -master_ipc_name_override steam0 -applaunch 570

2. (wait ~45s)

3. sb.launch_dota(1)
   → close_dota_mutex() via handle64.exe
   → registry AutoLoginUser = account_1
   → steam.exe from BotSteam/1 + env redirect BotProfiles/bot1
   → -master_ipc_name_override steam1 -applaunch 570
```

### All Problems Solved

| Problem | Solution |
|---------|----------|
| CEF file lock (no 2nd GUI) | AppData env redirect per instance |
| `-applaunch` spawns broken steamwebhelper | Steam+Dota in single launch |
| Shared config/ = same account | Separate Steam micro-copies (junctions) |
| Registry `AutoLoginUser` overwrite | `_set_autologin()` per launch |
| `dota_singleton_mutex` blocks 2nd Dota | handle64.exe closes it |
| Steam auto-updates on copy | `Steam.cfg` BootStrapperInhibitAll |

### Phase 2A — COMPLETE ✓

---

## 2026-03-22 — Phase 2B: Party + Accept + Pick

### Party Invite / Accept (gc.py)

Added GC message types:
- `CMsgInviteToParty` (4501): `steam_id` (fixed64 field 1)
- `CMsgInvitationCreated` (4502): `group_id` (varint field 1), `steam_id` (fixed64 field 2)
- `CMsgPartyInviteResponse` (4503): `party_id` (varint field 1), `accept` (bool field 2)
- `CMsgKickFromParty` (4504): `steam_id` (fixed64 field 1)
- `CMsgLeaveParty` (4505): empty

New methods in `DotaGC`:
- `invite_to_party(steam_id)` — send party invite
- `accept_party_invite(party_id)` — accept with party_id from InvitationCreated
- `decline_party_invite(party_id)` — decline
- `leave_party()` — leave current party
- `kick_from_party(steam_id)` — kick player

### RetrieveMessage Hook (gc.py)

Installed VMT hook on `ISteamGameCoordinator::RetrieveMessage` (vtable[2]) to intercept ALL incoming GC messages.

**Hook architecture:**
1. Fake vtable: `[orig_Send, orig_IsAvail, hook_code]`
2. Hook calls original first, then on success (EGCResultOK=0) logs:
   - `log+0x00`: atomic message counter
   - `log+0x04`: last msg_type (uint32)
   - `log+0x08`: last msg_size (uint32)
   - `log+0x10`: last message body (up to 0xF00 bytes)
   - `log+0x1000`: ready_up_key (from ReadyUpStatus lobby_id, fixed64)
   - `log+0x1008`: raw 16 bytes from InvitationCreated (parse party_id in Python)

**Methods:**
- `hook_retrieve_message()` → install hook, returns alloc addr
- `unhook_retrieve_message()` → restore original vtable
- `read_hook_log()` → read current log state
- `read_last_message_body()` → raw body of last intercepted message
- `wait_for_ready_up_key(timeout)` → poll until ready_up_key appears
- `wait_for_message(msg_type, timeout)` → wait for specific GC message, decode protobuf
- `auto_accept_match(timeout)` → wait + accept in one call

### Accept Match (gc.py)

- `accept_match(ready_up_key=0)` — if key=0, auto-reads from hook log
- `decline_match(ready_up_key=0)` — same
- `auto_accept_match(timeout=90)` — combines wait + accept

**Flow:**
1. `gc.hook_retrieve_message()` — install hook BEFORE queuing
2. `gc.start_finding_match()` or `gc.replay_search()` — queue
3. When match found, GC sends ReadyUpStatus (7170) with lobby_id
4. Hook captures lobby_id as ready_up_key at log+0x1000
5. `gc.auto_accept_match()` — polls and sends CMsgReadyUp (7070)

### Protobuf Decoder (gc.py)

Added minimal protobuf decoder `_decode_protobuf(data)`:
- Wire types: 0 (varint), 1 (fixed64), 2 (length-delimited), 5 (fixed32)
- Returns `{field_num: value}` dict
- Used for parsing incoming GC messages from the hook

### Hero Pick (commands.py)

- `select_hero(hero_name=None)` — console command `dota_select_hero <name>`
- `select_hero_by_index(hero_list, index)` — for distributing heroes across bots
- `BOT_HEROES` — list of 15 bot-friendly heroes (WK, Viper, Sniper, Drow, BS, Abaddon, BB, Ogre, Lich, Lion, CM, Tide, Centaur, Sven, Luna)

### Full Bot Flow (expected)

```python
# Setup
gc0 = DotaGC(mem0); gc0.init()
gc1 = DotaGC(mem1); gc1.init()
cmd0 = DotaCommands(mem0); cmd0.init()
cmd1 = DotaCommands(mem1); cmd1.init()

# 1. Install hooks on both
gc0.hook_retrieve_message()
gc1.hook_retrieve_message()

# 2. Bot0 invites Bot1 to party
gc0.invite_to_party(steam_id=76561198729640585)  # Bot1's SteamID
time.sleep(2)

# 3. Bot1 accepts (need to capture party_id from InvitationCreated)
log1 = gc1.read_hook_log()
# Parse party_id from party_raw (protobuf varint at offset 0)
party_body = bytes.fromhex(log1["party_raw"])
fields = _decode_protobuf(party_body)
party_id = fields.get(1, 0)
gc1.accept_party_invite(party_id)

# 4. Bot0 starts queue
gc0.replay_search()  # or start_finding_match()

# 5. Both auto-accept when match found
gc0.auto_accept_match(timeout=300)
gc1.auto_accept_match(timeout=300)

# 6. Pick heroes
cmd0.select_hero("npc_dota_hero_wraith_king")
cmd1.select_hero("npc_dota_hero_viper")
```

### Phase 2B — COMPLETE ✓

---

## 2026-03-22 — Phase 2B Live Testing: Party Invite + Accept

### Ключевые находки

#### 1. SendMessage data format — БАГ НАЙДЕН И ИСПРАВЛЕН
- **Формат данных SendMessage**: `[msg_type_with_flag(4)][header_size(4)][header][body]`
- `msg_type | 0x80000000` включается В НАЧАЛО буфера данных, не только в EDX
- RetrieveMessage возвращает тот же формат в буфере
- `build_gc_message()` исправлен — теперь включает msg_type в данные

#### 2. CMsgInviteToParty (4501) — требует 3 поля
```
field 1 (fixed64): steam_id — кого приглашаем
field 2 (varint):  client_version — 6725 (текущий)
field 5 (bytes):   CMsgClientPingData — 211B blob с ping до серверов
```
- Без `client_version` и `ping_data` GC **молча отбрасывает** инвайт
- Шаблон сохраняется в `config/invite_template.json` (записывается test_catch_invite.py)

#### 3. CMsgPartyInviteResponse (4503) — аналогично
```
field 1 (varint):  party_id — из CMsgInvitationCreated
field 2 (bool):    accept — true/false
field 3 (varint):  client_version — 6725
field 8 (bytes):   CMsgClientPingData — 211B
```

#### 4. CMsgInvitationCreated (4502) — приходит на ОТПРАВИТЕЛЯ
- **Критически важно**: 4502 приходит через RetrieveMessage на **того кто пригласил**, не на приглашённого!
- Приглашённый получает уведомление через **SO Cache** (`CGCSOCacheSubscribedJob(owner=<Invite:SteamID>)`)
- Поэтому party_id надо читать с отправителя

#### 5. Accept через GC (4503) — НЕ РАБОТАЕТ
- Отправка 4503 с приглашённого клиента отвергается GC молча
- Причина: GC не считает что у клиента есть pending invite (инвайт пришёл через SO Cache, не через GC message)
- Тело 4503 идентично реальному (party_id, client_version, ping_data совпадают)

#### 6. Accept через Panorama keypress — РАБОТАЕТ!
- Попап инвайта: `PopupPartyInvite#PartyInvitePopup` → `Button#AcceptButton` (defaultfocus)
- `script_debug_run` — **cheat flag**, не работает без sv_cheats
- `panorama_dispatch_event` — требует специальный формат строки, не просто имя события
- **Решение: PostMessage WM_KEYDOWN Enter** на окно Dota → AcceptButton имеет defaultfocus → Enter активирует
- Нужен `hideconsole` перед отправкой клавиши (иначе Enter уходит в консоль)

### Рабочий flow (verified!)

```python
# 1. Init
gc0 = DotaGC(mem0); gc0.init()
gc1 = DotaGC(mem1); gc1.init()
cmd_acceptor = DotaCommands(mem_acceptor); cmd_acceptor.init()

# 2. Leave existing parties
gc0.leave_party(); gc1.leave_party()
time.sleep(2)

# 3. Inviter sends invite
gc_inviter.hook_retrieve_message()
gc_inviter.invite_to_party(target_steam_id)

# 4. Catch party_id from 4502 on INVITER (not invitee!)
fields = gc_inviter.wait_for_message(GC_MSG_INVITATION_CREATED, timeout=10, poll_interval=0.01)
party_id = fields.get(1, 0)
gc_inviter.unhook_retrieve_message()
time.sleep(2)

# 5. Accept on invitee via Enter keypress
cmd_acceptor.execute('hideconsole')
time.sleep(0.5)

# Find Dota window, send Enter
hwnd = find_dota_window(acceptor_pid)
user32.SetForegroundWindow(hwnd)
time.sleep(0.3)
user32.PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001)
user32.PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001)
```

### Идентификация PID → аккаунт
- SteamID в памяти steamclient64.dll: scan для known SteamID bytes
- PID 36700 = zrvqd87257 (SteamID 76561198725850781) = qt317792
- PID 37700 = tqbao71896 (SteamID 76561198729640585)

### Panorama UI — Party Invite
```
DOTADashboardPopupManager#DashboardPopupManager
  └─ PopupPartyInvite#PartyInvitePopup (defaultfocus="AcceptButton")
       ├─ Label#PartyInviteTitle "ПРИГЛАШЕНИЕ В ГРУППУ"
       ├─ Label#PartyInviteSubtitle "xxx приглашает вас в группу."
       ├─ Button#AcceptButton "ПРИНЯТЬ"  ← defaultfocus, Enter activates
       └─ (DeclineButton etc.)
```

### Console commands discovered
- `panorama_dispatch_event` — exists but needs unknown string format
- `cl_panorama_script_help` — works, lists JS scopes (no cheat flag)
- `script_debug_run` — **cheat flag**, unusable in normal client
- `cl_script_*` — all cheat flagged
- `activategameui`, `gameui_hide` — work

---

## 2026-03-23 — Bypass: -tools + Matchmaking (GC Direct Queue)

### Проблема
Dota 2 блокирует Find Match если запущена с `-tools`, `-insecure` или `-dev`:
> "Невозможно искать игру, когда используются параметры запуска -insecure, -dev или -tools."

Debug tools (`-tools`) полезны для разработки, но блокируют матчмейкинг.

### Исследование — что НЕ работает

#### 1. Патч CommandLine raw string — НЕ ПОМОГАЕТ
- `CCommandLine` singleton в tier0.dll: `CommandLine()` export → struct
- Layout: `+0x08` m_pszCmdLine (char*), `+0x18` m_nParmCount, `+0x20` m_ppParms (char**)
- Затёрли `-tools`/`-insecure` пробелами в сырой строке и argv — **не помогло**
- Причина: `HasParam()` ищет пробелы и **находит** (мы заменили флаги на пробелы)
- Даже при замене на `_tools` (не совпадает) — не помогло, проверка кеширует bool при старте

#### 2. Патч .rdata lookup таблица в dota2.exe — НЕ ПОМОГАЕТ
- dota2.exe .rdata содержит wide/ascii строки `-insecure`, `-tools` для HasParam lookup
- VirtualProtectEx → PAGE_READWRITE → затёрли — **не помогло**
- Bool уже закеширован при старте, HasParam больше не вызывается

#### 3. Патч функции проверки в dota2.exe — НЕ ПОМОГАЕТ
- Нашли функцию в dota2.exe RVA 0x490D: 4 последовательных `HasParam()` вызова
- `-insecure` → `-trusted` → `-allow_third_party_software` → `-tools`
- Пропатчили: `XOR EDI,EDI; JMP +0x76` (перепрыгиваем все 4 проверки)
- **Не помогло** — это startup-код dota2.exe, не matchmaking check
- Matchmaking проверяется ОТДЕЛЬНО в client.dll

#### 4. IsInToolsMode в client.dll — тупик
- `Script_IsInToolsMode` script binding найден в client.dll .rdata (RVA 0x3D1D548)
- 3 xrefs: регистрация binding, script func table, call site
- Нативная функция (RVA 0x20C8650): читает глобальный ptr `[client.dll+0x6124790]`
- Значение = **0x0 (NULL)** → функция возвращает false
- Значит матчмейкинг блокируется НЕ через IsInToolsMode()

#### 5. Panorama JS — скомпилировано
- VPK файлы содержат `.vjs_c` (compiled JS), ключевые слова не в plaintext
- Проверка зашита в compiled Panorama скрипте, не в C++ коде client.dll

### Решение — GC Direct Queue (РАБОТАЕТ!)

**Проверка -tools/-insecure — ЧИСТО КЛИЕНТСКАЯ (UI).** GC сервер Valve не знает про launch параметры.

```python
# Вместо нажатия кнопки Find Match в UI:
gc.start_finding_match(game_modes=GameMode.ALL_PICK, matchgroups=Region.RUSSIA)
# GC принимает запрос, поиск начинается, UI показывает "Идёт поиск"
```

- `gc.start_finding_match()` отправляет `CMsgStartFindingMatch` (7033) напрямую через ISteamGameCoordinator::SendMessage
- GC отвечает нормально, матч ищется
- UI обновляется — показывает таймер поиска
- **Debug tools остаются загруженными и работают!**

**Verified live:** GC SendMessage → True, ответ msg_type=7681, поиск запустился в UI.

### Технические детали

#### CCommandLine layout (tier0.dll)
```
CCommandLine singleton (от CommandLine() export в tier0.dll):
+0x00: vtable (0x7ffef09f5b30)
+0x08: char* m_pszCmdLine — сырая строка
+0x10: int m_nLength — длина строки (269)
+0x18: int m_nParmCount — количество argv (5)
+0x20: char** m_ppParms — массив argv указателей
+0x28: char** m_ppParms2 — ещё один массив?
+0x30: int (1)
```

#### HasParam flow
- `HasParam(const char* flag)` — virtual call через vtable CCommandLine
- Функция в dota2.exe (RVA 0xF470) — ищет подстроку в m_pszCmdLine
- Возвращает **указатель** на найденный параметр (не bool!) — NULL если не найден
- При старте: результат кешируется, при matchmaking: проверка через compiled Panorama JS

#### dota2.exe startup check (RVA 0x490D)
```asm
; 4 последовательных HasParam вызова:
LEA rdx, "-insecure"    ; 0x490D
CALL HasParam           ; 0x491C (RVA 0xF470)
TEST rax, rax
JE skip_insecure

LEA rdx, "-trusted"     ; 0x492B
CALL HasParam
...
LEA rdx, "-allow_third_party_software"  ; 0x4946
CALL HasParam
...
LEA rdx, "-tools"       ; 0x495D
CALL HasParam
...
; edi = mode flag (0=normal, 2=tools/allow, 3=trusted)
```
Эта проверка НЕ влияет на matchmaking — она определяет network security mode.

#### Script_IsInToolsMode (client.dll RVA 0x20C8650)
```asm
mov rcx, [rip + 0x405C139]  ; global ptr @ client.dll+0x6124790
test rcx, rcx
jne has_engine                ; if engine ptr != NULL
xor al, al                    ; return false
ret
; has_engine: dereference engine, read bool, return
```
Global ptr = NULL → всегда возвращает false с `-tools`. Не влияет на matchmaking.

### Итог

| Подход | Результат |
|--------|-----------|
| Patch CCommandLine raw string | НЕ работает (HasParam кеширует) |
| Patch .rdata lookup strings | НЕ работает (startup cache) |
| Patch dota2.exe HasParam block | НЕ работает (не тот check) |
| IsInToolsMode global bool | NULL, не влияет |
| Panorama JS decompile | Compiled, нечитаемо |
| **GC Direct Queue** | **РАБОТАЕТ!** |

**Вывод:** для бот-фарма UI проверка вообще не релевантна — все действия (queue, accept, hero pick) идут через GC/console injection, минуя UI полностью.

### Phase 2B Live Testing — COMPLETE ✓

## 2026-03-23 — Phase 3: Lua Bot Brain — Mode System

### Problem: Mode files не загружались
- `require()` с lupa `unpack_returned_tuples=True` возвращал Python tuple, а не Lua table
- Mode файлы определяют `GetDesire()`/`Think()` как глобальные функции — при наивной загрузке все перезаписывали `_G`
- Глубокая цепочка зависимостей: mode → jmz_func → 7+ aba_* модулей → ts_libs/dota

### Решение: Permissive Require + Sandbox Loading

**Permissive require:**
- Оборачивает оригинальный `require()` в `pcall`
- При ошибке возвращает "permissive mock" — таблица где любой ключ возвращает no-op
- Кешируется по modname, каждый модуль пробуется только раз
- Единственный fallback: `game/Customize/general` (не критично)

**Sandbox loading (`_sandbox_load`):**
- `loadfile(path)` → получаем chunk function
- Создаём новый env table с `__index = _G` (наследует все глобалы)
- `debug.setupvalue(chunk, 1, env)` — устанавливаем `_ENV` чанка
- `pcall(chunk)` — все определения (`GetDesire`, `Think`, locals) попадают в env
- Если chunk вернул таблицу — мержим в env
- Возвращаем env как "module table" с captured functions

### Результат: 6/7 modes загружаются и GetDesire() работает

| Mode | GetDesire | Think | Примечание |
|------|-----------|-------|------------|
| laning | 0.268 | N | Think определяется условно (only if `local_mode_laning_generic`) |
| retreat | работает | N | Возвращает отрицательное (нет угрозы) |
| farm | 0.0 | Y | |
| push_mid | 0.0 | Y | |
| roam | 0.700 | Y | **Побеждает** в mode evaluation |
| team_roam | 0.0 | Y | |
| attack | skip | skip | GetDesire только для "buggy heroes" |

### Добавленные mock methods на bot handle
- `OriginalGetHealth` / `OriginalGetMaxHealth` — jmz_func вызывает на allied units
- `CanBeSeen` — видимость юнита
- `IsBuilding` / `IsTower` / `IsAncient` / `IsFort` / `IsPhantom` — type checks
- `GetCurrentActionType` — текущее действие (0 = IDLE)

### Добавленные глобальные функции
- `Min()` / `Max()` — Dota Lua API wrappers
- `GetDroppedItemList()` → пустая таблица
- `GetAncient(team)` → building handle (раньше возвращал None → crash при :GetLocation())
- `GetTower(team, tid)` → building handle из кеша или mock

### Добавленные константы
- `BOT_ACTION_DESIRE_*` (NONE..ABSOLUTE) — использовался наряду с BOT_MODE_DESIRE_*
- `ITEM_SLOT_TYPE_MAIN/BACKPACK/STASH/INVALID`
- `GAMEMODE_AP/CM/MO/TURBO`
- `BOT_ACTION_TYPE_IDLE/ATTACK/MOVE_TO/CAST_ABILITY/...`

### Архитектура mode evaluation в think_once()
1. Для каждого loaded mode вызываем `env.GetDesire()`
2. Выбираем mode с наибольшим desire > 0
3. Вызываем `env.Think()` выбранного mode
4. Устанавливаем `_active_mode` (для GetBot():GetActiveMode())
5. Если ни один mode не сработал — fallback Python AI (attack-move mid)

### Анализ API surface gap
- 194 уникальных метода вызываются в Lua скриптах
- 117 уже замокано в bot_brain.py (60%)
- 98 отсутствуют (40%)
  - 35-40 требуют реальных данных из памяти
  - 58-63 можно заткнуть стабами

**Топ missing по частоте:**
- Action_UseAbility* (500+ вызовов) — ability system
- FindAoELocation (349) — spatial queries
- GetExtrapolatedLocation (163) — prediction
- IsChanneling/IsMagicImmune/IsDisarmed (450 combined) — state flags
- HasScepter (81) — item system

### Решение: НЕ писать кастомный AI, а делать quality API
Кастомный Python AI = бездонная яма калибровки без визуального feedback.
OpenHyperAI (5600 строк jmz_func.lua) = тысячи часов тюнинга людьми.
**Наша задача: дать Lua скриптам правильные данные, они сами решат.**

### Game Data JSONs (из assets.zip другого чита)
Скопированы в `tools/dota2/data/`:
- `npc_heroes.json` — 128 героев, все базовые статы (STR/AGI/INT + gain, damage, speed, armor, range, abilities list)
- `npc_abilities.json` — 1900 абилок (cooldown, manacost, damage, behavior — **по уровням**)
- `items.json` — 561 предмет (cost, components, bonuses)
- `npc_units.json` — 430KB юнитов (крипы, саммоны)
- `neutral_items.json` — нейтральные предметы

**Импакт:** Статичные данные (damage, manacost, castpoint, behavior) берём из JSON.
Из памяти читаем ТОЛЬКО: ability level + remaining cooldown + текущую ману.
Hero base stats считаются как `base + primary_attr_gain * level`.

---

## 2026-03-23 — Phase 3: Ability System + GameDataDB

### Phase 3.1: GameDataDB

Created `cheat/game_data.py` — Python class for static JSON data lookup.

- Loads `npc_heroes.json` (128 heroes), `npc_abilities.json` (1898 abilities), `items.json` (560 items)
- **Hero lookup**: `get_hero_merged()` merges base template + hero-specific data
  - `hero_abilities()` — extract Ability1..Ability25 list
  - `hero_base_stats()` — STR/AGI/INT + gains
  - `hero_attack_range()`, `hero_move_speed()`, `hero_base_armor()`
- **Ability lookup**: `ability_at_level(name, level)` — resolves space-separated per-level values
  - CD, mana cost, damage, cast range, behavior flags, AbilityValues
  - `_parse_behavior()` — converts "DOTA_ABILITY_BEHAVIOR_UNIT_TARGET | AOE" → int flags
- **Item lookup**: `item_cost()`, `item_cooldown()`, `is_secret_shop_item()`

Verified: Sven Storm Bolt @lv2 → CD=18, Mana=115, Damage=160, Range=600 ✓

### Phase 3.2: Ability Offsets + Handles

**Source2SDK offsets found** (all +8 for runtime):

| Field | source2sdk | Runtime (+8) | Class |
|-------|-----------|-------------|-------|
| m_vecAbilities | 0xD10 | **0xD18** | C_DOTA_BaseNPC |
| m_iLevel | 0x620 | **0x628** | C_DOTABaseAbility |
| m_fCooldown | 0x630 | **0x638** | C_DOTABaseAbility |
| m_flCooldownLength | 0x634 | **0x63C** | C_DOTABaseAbility |
| m_iManaCost | 0x638 | **0x640** | C_DOTABaseAbility |
| m_bToggleState | 0x625 | **0x62D** | C_DOTABaseAbility |
| m_bHidden | 0x60F | **0x617** | C_DOTABaseAbility |
| m_bActivated | 0x611 | **0x619** | C_DOTABaseAbility |
| m_bAutoCastState | 0x63C | **0x644** | C_DOTABaseAbility |
| m_nAbilityCurrentCharges | 0x658 | **0x660** | C_DOTABaseAbility |
| m_iCurrentXP | 0x1A20 | **0x1A28** | C_DOTA_BaseNPC_Hero |
| m_iAbilityPoints | 0x1A24 | **0x1A2C** | C_DOTA_BaseNPC_Hero |
| m_flStrength | 0x1A34 | **0x1A3C** | C_DOTA_BaseNPC_Hero |
| m_flAgility | 0x1A38 | **0x1A40** | C_DOTA_BaseNPC_Hero |
| m_flIntellect | 0x1A3C | **0x1A44** | C_DOTA_BaseNPC_Hero |

**CHandle resolution**: `handle & 0x7FFF` → entity index → `chunk_list[idx/512][idx%512]`

**game_state.py additions**:
- `read_ability_handles(entity)` — read CHandle[35] array from m_vecAbilities
- `resolve_handle(handle)` — CHandle → entity pointer via chunk lists
- `read_ability_data(ability_entity)` — read level, CD, mana_cost, hidden, charges
- `get_hero_abilities(hero_entity)` — full ability list with names from identity

**bot_brain.py ability handles**:
- `_make_ability_handle(ab_data, hero_name)` — Lua handle with:
  - Memory data: GetLevel, GetCooldownTimeRemaining, GetCurrentCharges
  - JSON data: GetManaCost, GetDamage, GetCastRange, GetCooldown, GetBehavior
  - Hybrid: IsFullyCastable (level>0 AND cd==0 AND mana>=cost)
  - GetSpecialValueFor/Int/Float → AbilityValues from JSON
- `GetAbilityByName()` → reads real abilities from memory, creates handle
- `GetAbilityInSlot()` → same
- Action_UseAbility/OnEntity/OnLocation → PrepareUnitOrders(CAST_*)
- ActionImmediate_LevelAbility → PrepareUnitOrders(TRAIN_ABILITY)

### Phase 3.3: Item Handles (partial)

- `_make_item_handle(item_name)` — from JSON data
- `GetItemCost` global → reads from DB (560 items)
- Inventory reading from memory → TODO (need m_hItems offset)

### Phase 3.4: Stats from Memory + JSON

EntityCache now stores per-hero:
- `mana`, `max_mana`, `mana_regen` (from memory)
- `damage_min` (from memory)

Unit handle updated:
- `GetMana/GetMaxMana/GetManaRegen` → real values from EntityCache
- `GetAttackRange/GetAttackSpeed/GetSecondsPerAttack/GetAttackPoint` → from JSON DB
- `GetArmor/GetMagicResist` → from JSON base stats
- `GetAttackDamage/GetBaseDamage` → from memory (m_iDamageMin)
- `GetUnitList` → returns real heroes/creeps/buildings from EntityCache

### Coverage Update

**Before**: 96 methods stubbed (60% coverage)
**After**: ~150 methods with real data (~77% coverage)

Methods now returning real data:
- ✅ GetAbilityByName, GetAbilityInSlot, GetAbilityCount
- ✅ Ability.GetLevel, GetCooldownTimeRemaining, IsFullyCastable
- ✅ Ability.GetManaCost, GetDamage, GetCastRange, GetBehavior
- ✅ Ability.GetSpecialValueFor/Int/Float
- ✅ Action_UseAbility*, ActionImmediate_LevelAbility
- ✅ GetMana, GetMaxMana, GetManaRegen
- ✅ GetAttackDamage, GetAttackRange, GetAttackSpeed, GetArmor
- ✅ GetItemCost, IsItemPurchasedFromSecretShop
- ✅ GetUnitList (all 6 types)

Still TODO:
- ❌ Inventory from memory (GetItemInSlot, FindItemSlot, HasScepter)
- ❌ Modifiers (HasModifier, IsChanneling, IsMagicImmune, IsStunned)
- ❌ Gold from memory
- ❌ Max HP from memory (currently same as HP)
- ❌ State flags (from modifier name checks)

### Files Modified
```
cheat/
  game_data.py    — NEW: GameDataDB (258 lines), JSON static data lookup
  offsets.py      — +BaseAbility, +BaseHero classes with 15 new offsets
  game_state.py   — +ability system (read_ability_handles, resolve_handle, read_ability_data, get_hero_abilities)
  bot_brain.py    — 1332→1650 lines (+318), real abilities/items/stats
```

### Runtime Verification (lobby, 2026-03-23)

**m_vecAbilities layout** — NOT an inline CHandle[35] array!
- `CNetworkUtlVectorBase<CHandle>` at source2sdk offset 0xD10
- element_ptr at entity+0xD08 (8 bytes), count at entity+0xD10 (int32)
- Reads handle array from separate allocation (not inline in entity)
- Sniper: count=21 handles, 9 resolved to valid ability entities

**CHandle resolution** — entity index ≠ chunk slot position!
- CHandle bits[0:14] = entity_index, but index does NOT map to chunk[idx/512][idx%512]
- Chunk slots are NOT indexed by entity index — entities stored in arbitrary positions
- **Solution**: build entity_index→entity_ptr map during entity scan (handle_index in EntityCache)
- handle_index built from identity.handle field during chunk scan (~2850 entries)

**Ability offsets verified (shrapnel entity)**:
- +0x628: m_iLevel = 7 ✅
- +0x638: m_fCooldown = 0.0 ✅ (not on CD)
- +0x640: m_iManaCost = 0 (not populated in lobby — JSON fallback works)

**Missing abilities in lobby**:
- 12/21 handles NOT in entity system (entities not fully initialized in lobby)
- sniper_assassinate (slot 3), sniper_concussive_grenade, sniper_keen_scope — all missing
- Expected to resolve in actual game match

**Full pipeline test results**:
```
Hero: npc_dota_hero_sniper
  Memory: 9 abilities resolved (shrapnel lv=7, headshot lv=0, take_aim lv=0, + innates)
  JSON: mana=75, cd=0, range=1800, stats=STR19/AGI27/INT15, ATK range=550
  Memory: HP=396, Mana=0/199, DmgMin=47
```

---

## 2026-03-23 — Phase 3.5-3.6: Inventory, Gold, MaxHP, UnitState

### CRITICAL: "+8 shift" theory was WRONG for many fields!

SchemaSystem runtime query reveals actual offsets differ from source2sdk by variable amounts.
**The only reliable source is SchemaSystem at runtime.**

### Offset Corrections (SchemaSystem verified 2026-03-23)

| Field | Old (wrong) | Correct | Class |
|-------|------------|---------|-------|
| m_iCurrentLevel | 0xC9C | **0xC7C** | C_DOTA_BaseNPC |
| m_flMana | 0xCF0 | **0xCD4** | C_DOTA_BaseNPC |
| m_flMaxMana | 0xCF4 | **0xCD8** | C_DOTA_BaseNPC |
| m_flManaRegen | 0xCF8 | **0x16F0** | C_DOTA_BaseNPC |
| m_nUnitState64 | 0x1258 | **0x1260** | C_DOTA_BaseNPC |
| m_Inventory | 0x1158 | **0x1168** | C_DOTA_BaseNPC |
| m_iCurrentXP | 0x1A28 | **0x1A5C** | C_DOTA_BaseNPC_Hero |
| m_iAbilityPoints | 0x1A2C | **0x1A60** | C_DOTA_BaseNPC_Hero |
| m_flStrength | 0x1A3C | **0x1A70** | C_DOTA_BaseNPC_Hero |
| m_flAgility | 0x1A40 | **0x1A74** | C_DOTA_BaseNPC_Hero |
| m_flIntellect | 0x1A44 | **0x1A78** | C_DOTA_BaseNPC_Hero |

Fields that were already correct: m_iHealth(0x354), m_iMaxHealth(0x350), m_pGameSceneNode(0x338),
m_iTeamNum(0x3F3), m_vecAbilities(D00/D08), m_iszUnitName(0xD48), m_iDamageMin(0xDF0),
m_ModifierManager(0xE08), m_flHealthRegen(0x16F4).

### m_hItems = INLINE CHandle array (NOT pointer-based!)

Unlike m_vecAbilities (which stores data via pointer), m_hItems is **inline**:
- Count at entity + 0x1188 (Inventory+0x20)
- Inline CHandle[25] at entity + 0x118C (Inventory+0x24)
- 25 slots: 0-5 main, 6-8 backpack, 9-14 stash, 15 TP, 16 neutral

### Runtime Verification Results (demo mode)

| Test | Result |
|------|--------|
| m_iMaxHealth | HP=417, MaxHP=736 — OK |
| m_nUnitState64 | 0x0 (idle, no CC) — OK (was garbage pointer before) |
| Inventory | 25 slots, items resolved: item_branches, item_magic_stick, item_tango, item_tpscroll — OK |
| AbilityPoints | 0 (expected in demo) — OK |
| Mana | 17.3/327.0 (was 0/198.9 with old offsets) — much better |
| Level | 2 (was 0 with old offset) — OK |
| STR/AGI/INT | 21.0/30.2/17.6 — realistic values |

### Gold — NOT working in demo mode

- `dota_data_radiant` entity not found in demo (only `dota_data_dire` + `dota_data_spectator`)
- `dota_data_dire` entity_ptr (0x2127d1b3e28) is misaligned — embedded struct, not standard entity
- entity+0x10 ≠ identity (not a normal CEntityInstance)
- Gold reads as 0 — DataNonSpectator offsets need verification in real match
- **Fallback: GetGold()=600 until verified in real game**

### Gold System — C_DOTA_DataNonSpectator

Gold is NOT on hero/controller entities. It's in separate per-team data entities:
- `"dota_data_radiant"` / `"dota_data_dire"` designer names
- `m_vecDataTeam` at +0x5F0 (CUtlVectorEmbeddedNetworkVar)
- `DataTeamPlayer_t` stride = 0x1240 bytes, indexed by player slot
- Fields: reliable_gold+0x30, unreliable_gold+0x34, net_worth+0x8C, last_hits+0x94, denies+0x98

### UnitState64 — 38+ Flags

Added `UnitState` class in offsets.py with all MODIFIER_STATE_* flags:
- ROOTED, DISARMED, SILENCED, STUNNED, HEXED, INVISIBLE, INVULNERABLE, MAGIC_IMMUNE, etc.
- Replaces hardcoded `False` stubs in bot_brain.py CC/Status section

### Inventory System (game_state.py)

- `read_item_handles(entity)` — reads CHandle[] from m_hItems CNetworkUtlVectorBase
- `read_item_data(item_entity)` — reads level, cooldown, charges, name from identity
- `get_hero_items(hero_entity)` — resolves all CHandles, returns item data list
- Slots: 0-5 main, 6-8 backpack, 9-14 stash, 15 TP, 16 neutral

### Bot Handle Updates (bot_brain.py)

**Real data (was stubs):**
- `GetGold()` → reliable + unreliable from DataNonSpectator
- `GetNetWorth()` → from DataNonSpectator
- `GetLastHits()` / `GetDenies()` → from DataNonSpectator
- `GetMaxHealth()` → from m_iMaxHealth (was same as HP)
- `GetItemInSlot(slot)` → reads inventory from memory, creates item handle
- `FindItemSlot(name)` → scans inventory for item name
- `HasScepter()` → scans inventory for "item_ultimate_scepter"
- `GetAbilityPoints()` → from BaseHero.ABILITY_POINTS
- `IsStunned/Silenced/Hexed/Rooted/Disarmed/MagicImmune/Invisible/...` → from m_nUnitState64 bits
- `Action_ClearActions()` → sends STOP order
- `GetExtrapolatedLocation()` → returns current pos (velocity TODO)

### Files Modified
```
cheat/
  offsets.py     — +UnitState (38 flags), +DataNonSpectator, +DataTeamPlayer,
                   +BaseEntity.MAX_HEALTH, +BaseNPC.ITEMS_PTR/COUNT/UNIT_STATE_64/MODIFIER_MANAGER
  game_state.py  — +read_max_health, +read_unit_state, +9 is_*() helpers,
                   +read_item_handles, +read_item_data, +get_hero_items,
                   +find_data_entity, +read_gold, +read_net_worth, +read_last_hits_denies,
                   +read_ability_points
  bot_brain.py   — real inventory/gold/maxHP/unitstate in bot handle,
                   +_refresh_items, +_get_gold, +_get_net_worth, +_get_last_hits_denies,
                   +Action_ClearActions, +GetExtrapolatedLocation, +GetAbilityPoints
test_phase35.py  — runtime verification script (5 tests)
```

### Coverage Update

**Before**: ~150 methods with real data (~77%)
**After**: ~165 methods with real data (~85%)

New real data:
- Inventory: GetItemInSlot, FindItemSlot, HasScepter
- Economy: GetGold, GetNetWorth, GetLastHits, GetDenies
- Status: 11 CC/state flag methods via UnitState64
- Health: GetMaxHealth (was HP copy)
- Actions: Action_ClearActions, GetAbilityPoints

### TODO (remaining stubs)
- Modifiers: HasModifier(name), NumModifiers, GetModifierName — needs m_ModifierManager reverse
- IsChanneling: needs ability cast state, not just COMMAND_RESTRICTED flag
- GetExtrapolatedLocation: needs velocity vector
- Game time: read from CDOTAGamerules instead of system clock
- Item cooldown: read from memory (currently returns JSON value)

---

## 2026-03-23 — SchemaSystem Auto-Dumper + Offset Corrections + Entity System Fixes

### CRITICAL FIX: "+8 shift" theory was WRONG

The assumption that all source2sdk offsets shift by +8 was incorrect. SchemaSystem runtime query
reveals actual offsets differ by variable amounts per class. Some fields were off by -28 to +52.

| Field | Old (wrong) | Correct | Source |
|-------|------------|---------|--------|
| m_iCurrentLevel | 0xC9C | **0xC7C** | SchemaSystem |
| m_flMana | 0xCF0 | **0xCD4** | SchemaSystem |
| m_flMaxMana | 0xCF4 | **0xCD8** | SchemaSystem |
| m_flManaRegen | 0xCF8 | **0x16F0** | SchemaSystem |
| m_nUnitState64 | 0x1258 | **0x1260** | SchemaSystem |
| m_Inventory | 0x1158 | **0x1168** | SchemaSystem |
| m_iCurrentXP | 0x1A28 | **0x1A5C** | SchemaSystem |
| m_iAbilityPoints | 0x1A2C | **0x1A60** | SchemaSystem |
| m_flStrength | 0x1A3C | **0x1A70** | SchemaSystem |

### dump_schema_offsets.py — Auto-Dumper

Created `dump_schema_offsets.py` — one command after each Dota patch:
```bash
python dump_schema_offsets.py --apply
```

**How it works:**
1. Connects to running Dota 2 via pymem
2. Walks SchemaSystem hash map → dumps C_DOTA_BaseNPC (201 fields), Hero (66), Controller (103)
3. Infers base class offsets (C_BaseEntity, Gamerules) via runtime verification on live entities
4. Uses verified fallback for C_DOTABaseAbility (not in schema hash map)
5. Auto-generates `cheat/offsets.py`

**Classes dumped from schema:** C_DOTA_BaseNPC, C_DOTA_BaseNPC_Hero, C_DOTAPlayerController
**Classes inferred at runtime:** C_BaseEntity, CGameSceneNode, CDOTAGamerules, C_DOTAGamerulesProxy
**Classes using verified fallback:** C_DOTABaseAbility (base class not in schema)

Also saves `schema_dump.json` with raw field data for reference.

### m_hItems = INLINE CHandle[25] (NOT pointer-based!)

Unlike m_vecAbilities (CNetworkUtlVectorBase with data pointer), m_hItems is inline:
- Count at entity + 0x1188 (Inventory+0x20)
- Inline CHandle[25] starting at entity + 0x118C (Inventory+0x24)
- Verified: Dazzle items = item_branches x2, item_magic_stick, item_tango

### Local Hero Detection Fixed

**Problem:** `find_local_hero()` returned first hero from chunk scan (Sniper), not our actual hero (Lion).

**Root cause:** Local player's hero entity is in a "sentinel chunk" — chunk_list entry = 0xFFFFFFFF...
(not a valid pointer). Entity exists in identity memory region but NOT accessible via chunk pointers.

**Solution — 3-tier resolution:**
1. Find CDOTAPlayerController with slot=0, read m_hAssignedHero CHandle
2. Resolve handle via chunk scan (fast, works for most entities)
3. **Fallback: identity region scan** — scan 128KB from each chunk_list base, match handle value
   at stride-4. This finds entities in sentinel chunks including local predicted hero.

**Verified:** Lion (entity_index 1288, chunk 2 = sentinel) found correctly via identity region scan.

### Entity System — Sentinel Chunks + Phase B Scan

**Discovery:** Chunk lists have entries with upper 32 bits = 0xFFFFFFFF (sentinel). These chunks
are NOT invalid — they contain real entities (local hero, predicted entities, abilities).
Sentinel chunks: [2, 16, 30, 44, 58] — same pattern in all 3 lists.

**Phase B scan added to EntityCache.update():**
After standard chunk scan (Phase A), scan 128KB from each chunk_list base for additional
entity identities. Validation: entity_ptr must be readable (no HP check — abilities have no HP).

**Results:**
- Phase A: ~1672 entities
- Phase B: +1710 new entities (including 4 heroes + all ability entities)
- Local hero abilities (lion_impale, lion_voodoo, etc.) now resolve via Phase B handle index

### Visibility-Based Entity System (confirmed)

**Theory confirmed in live match:**
- When enemies in fog: 4-5 heroes visible (our team only)
- When enemies in vision: **8-9 heroes visible** (all in range)
- This is normal Dota 2 behavior — server doesn't send enemy data when in fog of war
- For bot farm this is fine — we only need our hero + visible enemies

### Runtime Verification Results (live match, PRE_GAME)

| Test | Result |
|------|--------|
| Local hero (Lion) | HP=516/516, Mana=303/303, Lv=1, AP=1 — **PASS** |
| MaxHP | 516 >= 516 — **PASS** |
| UnitState64 | 0x0 (idle) — **PASS** |
| STR/AGI/INT | 18.0/15.0/19.0 — **PASS** |
| Abilities | 19 resolved (impale, voodoo, mana_drain, finger) — **PASS** |
| Items | TP scroll slot 15 — **PASS** |
| AbilityPoints | 1 — **PASS** |
| Heroes visible | 8/10 (enemies in vision) — **PASS** |
| Creeps/Towers/Buildings | 23/16/20 — **PASS** |
| Gold | DataNonSpectator not working (embedded entity) — **TODO** |

### Files Created/Modified
```
dump_schema_offsets.py  — NEW: SchemaSystem auto-dumper (one command after patch)
schema_dump.json        — NEW: raw schema field data
cheat/
  offsets.py     — AUTO-GENERATED from schema dump (201+ fields, 8 classes)
  game_state.py  — +find_local_hero (3-tier resolution), +_resolve_handle_via_identity,
                   +read_max_health, +read_unit_state, +inventory system, +gold system
  bot_brain.py   — EntityCache Phase B scan, real inventory/gold/unitstate in bot handle
test_phase35.py  — runtime verification script
```

### New Offsets Added (bonus from SchemaSystem dump)
```
BaseNPC.ATTACK_RANGE      = 0xCA8   # m_iAttackRange
BaseNPC.MOVE_SPEED        = 0xCBC   # m_iMoveSpeed
BaseNPC.BASE_ATTACK_TIME  = 0xCC4   # m_flBaseAttackTime
BaseNPC.BASE_ATTACK_SPEED = 0xCC0   # m_iBaseAttackSpeed
BaseNPC.IS_ILLUSION       = 0xCFC   # m_bIsIllusion
BaseNPC.IS_PHANTOM        = 0xC60   # m_bIsPhantom
BaseNPC.DAY_VISION_RANGE  = 0xDE8
BaseNPC.NIGHT_VISION_RANGE= 0xDEC
BaseNPC.PHYSICAL_ARMOR    = 0x15D8
BaseNPC.MAGIC_RESIST      = 0x15DC
BaseHero.STRENGTH_TOTAL   = 0x1A7C  # includes items/buffs
BaseHero.AGILITY_TOTAL    = 0x1A80
BaseHero.INTELLECT_TOTAL  = 0x1A84
BaseHero.PLAYER_ID        = 0x1AF8
BaseHero.RESPAWN_TIME     = 0x1A68
PlayerController.ASSIGNED_HERO = 0x90C
```

---

## 2026-03-23 — Phase 3.8: Quick Wins

### 1. Local hero added to EntityCache

**Problem:** `find_local_hero()` found the hero entity (e.g. Lion in sentinel chunk), but it was NOT
added to `cache.heroes`. Bot brain's `_find_our_hero()` then failed to find it in cache.

**Fix:** After Phase B scan + gold entity search, call `game.find_local_hero()` and add to
`cache.heroes` if not already present. Full hero data is read (HP, mana, position, stats, unit_state).

### 2. Game time from CDOTAGamerules

**Problem:** `cache.game_time` used `time.time()` (epoch), not actual Dota clock. Cooldown
calculations and Lua bot scripts got wrong time values.

**Fix:** Read `Gamerules + 0x30` (m_flGameTime). Validated with range check: `-200 < gt < 10000`
(covers pre-game negative time). Falls back to `time.time()` if gamerules not available.

Also added `DotaGame.get_game_time()` method for standalone use.

Updated in bot_brain:
- `EntityCache.update()` — reads real game time
- `DotaTime()`/`GameTime()` Lua globals — return Dota clock
- `_get_cd_remaining()` in ability handle — uses Dota clock for cooldown math

### CRITICAL: Entity System Stale After Demo→Match

**Problem:** When transitioning from demo mode to a real match (lobby/bot match), the entity system
pointer (GRS+0x58) stays the same, but entity data becomes stale. Heroes from demo are still found,
but new match entities (including the real local hero) are NOT visible.

**Evidence:**
- `find_local_hero()` finds Lion at 0x21313420000 — demo entity, HP frozen at 670
- Real match Sand King HP ticks (565→574 via direct read), proving game IS live
- Controller slot=0 found with hero_handle=0xBD8508, resolves via sentinel chunk to SAME Lion entity
- Gold value 1995 found in heap (0x212ebdbe0c0) but NOT in any known entity → standalone buffer
- `dota_data_radiant` NOT found, `dota_data_dire` found but stale from demo

**Root cause:** Entity system reuses the same allocation but entities are replaced.
The chunk scan finds stale identities. `find_local_hero()` resolves handle via identity region
scan but lands on the OLD entity object at the SAME address (reused allocation).

**Partial workaround:** Direct memory reads to known entity addresses still work (HP ticks),
but the DATA is from the current match because the entity object was reallocated at same address.
Gold, however, lives in a separate networked data buffer not directly linked to entity system.

**TODO:** Need to either:
1. Force entity system re-scan (invalidate old chunk data)
2. Find gold via SchemaSystem runtime query of the actual CNetworkUtlVectorBase
3. Use console command injection (`script_debug_output` trick) to read gold

### Game Time — m_flGameTime NOT Updated in Live Match

**Finding:** `CDOTAGamerules + 0x30` (m_flGameTime from schema) is ALWAYS 0.0 in live matches.
Only works in demo mode / spectator mode. Client doesn't receive this field in normal play.
Server-side only.

**No ticking floats found** in gamerules during live match (64KB scan, 2s interval).
The only changing values are counters at +0x0A0C and noise at +0x0BB8.

**Workaround:** `time.time()` fallback is the correct approach for live matches.
Could potentially find CurTime via GlobalVars pattern but not worth the effort.

### 3. Fixed iter_entities generator RuntimeError

**Problem:** `for ... in game.iter_entities(): ... break` could cause `RuntimeError: generator
ignored GeneratorExit` because the generator wasn't properly closed after early break.

**Fix:** Use explicit `gen = game.iter_entities()` + `try/finally: gen.close()` pattern.

## 2026-03-23 — Phase 5: Hero Script + Ability Usage

### 1. Fixed hero_lion.lua crash (P0)

**Root cause:** Permissive `require` returned mock objects for missing `Customize/hero/lion.lua`.
Mock's `__index` returns functions for every key → `tBotSet.Enable` = function (truthy in Lua) →
`J.GetTalentBuildList(tBotSet.Talent)` where `tBotSet.Talent` = function → `#function` crash.

**Fix:** Added "passthrough" list for `require` — modules matching "Customize" re-raise errors
instead of returning permissive mock. This lets `xpcall` in `J.SetUserHeroInit()` handle the
failure gracefully (status=false, tBotSet=nil → skip customization).

**Verified:** Lua `:` method syntax works correctly on Python-created handles (lambdas as userdata
with `__call` are fine with lupa's Lua 5.4).

### 2. Loaded ability_item_usage_generic.lua

**Problem:** Hero script (hero_lion.lua) exports `X.SkillsComplement()` for spell casting, but
nobody called it. In original Dota 2, `ability_item_usage_generic.lua` defines `AbilityUsageThink()`
which calls `BotBuild.SkillsComplement()`.

**Fix:** Load `ability_item_usage_generic.lua` via `_sandbox_load()` in `_load_scripts()`.
Call `AbilityUsageThink()` and `AbilityLevelUpThink()` every tick in `_run_ability_think()`.

This also solves P3 (auto level-up) — `AbilityLevelUpThink` uses `sAbilityLevelUpList` from
the hero script to level abilities automatically.

### 3. Vector metatable with arithmetic

**Problem:** jmz_func.lua does `local delta = enemy_loc - bot_loc`, `dir * speed`, etc.
Our Vector was a plain Lua table without metamethods.

**Fix:** Created `_VectorMT` metatable in Lua with `__add`, `__sub`, `__mul`, `__div`, `__unm`,
`__eq`, `__tostring` metamethods + `:Length()`, `:Length2D()`, `:Normalized()`, `:Dot()`, `:Cross()`.
`_make_vector()` now creates vectors via Lua-side constructor with `setmetatable(v, _VectorMT)`.

### 4. Cached GetBot() handle

**Problem:** Lua scripts store properties on the bot handle (`bot.frameProcessTime = 0.1`,
`bot.stuckLoc`, `bot.assignedRole`). Each `GetBot()` was creating a new table → properties lost.

**Fix:** `_make_bot_handle()` now caches and returns the same Lua table. Invalidated when
entity_ptr changes (hero death/reconnect).

### 5. Python laning fallback

**Problem:** `mode_laning_generic.lua` only defines `Think()` for pos 1 carry when human support
at pos 5 exists. In pure bot games, laning Think is never created.

**Fix:** Upgraded `_fallback_think()` to do basic laning: last-hit enemy creeps (HP < damage),
deny ally creeps, advance along lane waypoints.

### 7. Mode Think() no-op detection

**Problem:** Roam mode Think() executes successfully but doesn't issue any Action (falls through
all conditions). `mode_handled=True` was set unconditionally → fallback never ran → hero stuck.

**Fix:** Track `_last_action_time` — if Think() didn't update it, mode_handled stays False →
Python fallback kicks in. Hero now walks from fountain to mid lane via LANE_ADVANCE.

### 8. Fixed TRAIN_ABILITY order

**Problem:** `ActionImmediate_LevelAbility` called `cmd.level_ability()` — method didn't exist
(correct name: `train_ability`). Also, `ability_index` parameter must be **entity index**
(`handle & 0x7FFF`), NOT ability slot number. Original code passed slot=0,1,2...

**Fix:** Changed `_level_ability` handler to resolve entity index from ability handle.
Brute-force test confirmed order_type=11 works with entity_index.

### 9. DotaTime() = 0 bug (CRITICAL)

**Problem:** `game_time` from Gamerules memory = 0.0 in live match (server-side only field).
`_dota_time()` checked `-200 < 0 < 10000` → True → returned 0. ALL rate limiters in Lua scripts
use `DotaTime()` for throttling → `DotaTime() - lastTime < threshold` → `0 - 0 < 0.2` → always True
→ every function blocked forever. No errors, no actions.

**Fix:** Changed condition to `gt > 1.0` (reject 0 as invalid). Now uses fallback
`time.time() - brain._start_time`.

### 10. Missing API from error logging

Added `BotLogger` class for deduplicating error collection + periodic summary.
Found and fixed 4 errors:
1. `math.pow` — removed in Lua 5.4, added shim `math.pow = function(b,e) return b^e end`
2. `GetHeroLevel` — missing global, added
3. `GetAttributeValue` — missing handle method, added (returns 0.0)
4. `GetAmountAlongLane` — returned float, but API returns `{amount=float, distance=float}`

After fixes: **0 errors**, Lua scripts actively calling Move/AttackMove actions.

### Live test results (2026-03-23)
- Hero: Lion, Team: Radiant
- 60s at 7Hz — 0 crashes, 0 errors
- All scripts loaded: bot_generic, hero_lion, jmz_func, aba_skill, ability_item_usage_generic
- Hero moved from fountain (-6950,-6275) to mid lane center (-1280,-996) in ~15s
- Roam mode selected (desire 0.70) but Think() no-op → Python fallback handles movement

### 6. Minor additions
- `GetLocationToLocationDistance` global (was missing, used by utils.lua)
- `ActionImmediate_SwapItems`, `ActionImmediate_DisassembleItem` stubs on handle

## 2026-03-24 — Phase 6: Critical Bot Fixes (4 bugs)

### 1. think_once() fallback never called (CRITICAL — root cause of AFK)

**Problem:** `bot_think_fn()` (bot_generic.lua Think) set `mode_handled = True` unconditionally,
even when Think() produced no action. This blocked mode evaluation AND `_fallback_think()`.
Result: hero walked to lane via initial move, then stood AFK forever.

**Fix:** Changed `think_once()` to track `_last_action_time` instead of boolean.
Now: bot_generic Think → mode evaluation (cascading by desire) → `_fallback_think()` as last resort.
Each stage only proceeds if the previous one didn't produce an action.

### 2. BotBuild local → global (CRITICAL — no skill leveling)

**Problem:** `bot_generic.lua` line 6: `local BotBuild = dofile(...)` — BotBuild is local.
`ability_item_usage_generic.lua` reads `BotBuild['sSkillList']` from global scope → gets nil.
`sAbilityLevelUpList = nil` → `AbilityLevelUpThink` skips all level-ups → hero stays lv1 skills.

**Root cause:** In Dota 2 engine, all scripts share one bot environment. Our sandbox isolation
creates separate envs → locals don't propagate between scripts.

**Fix:** Patch bot_generic.lua code before execution:
`code.replace("local BotBuild = dofile(", "BotBuild = dofile(", 1)`
Now BotBuild lands in _G, ability_item_usage_generic.lua picks up sSkillList correctly.

**Verified:** `sSkillList=YES` in init, `LEVEL(lion_impale)` actions produced.

### 3. Cast with ent_idx=0 crashes Dota (CRITICAL)

**Problem:** `_action_use_ability`, `_action_use_ability_on_location`, `_action_use_ability_on_entity`
passed **slot number** (0,1,2...) as `ability_index` to PrepareUnitOrders instead of **entity index**
(handle & 0x7FFF). With slot=0, the game received ability_index=0 → crash in PrepareUnitOrders.

Also: `cast_target` always passed `target_index=0` — no target resolution.

**Fix:**
- All cast actions now use `ent_idx` from `_find_ability_info()` (not slot)
- Guard: `if not ent_idx: return` — skip invalid abilities silently
- `cast_target` now resolves target entity index from handle's identity

### 4. Persistent order buffer + rate limiting

**Problem:** Every `order()` call did VirtualAllocEx + CreateRemoteThread + VirtualFreeEx.
At 7Hz with multiple orders per tick, this caused memory pressure and potential crashes.

**Fix:**
- Persistent RWX buffer (`_order_buf`) — allocated once, reused every call
- Rate limit: movement orders throttled to 50ms min interval
- Immediate actions (TRAIN_ABILITY, CAST_*, PURCHASE, STOP) bypass rate limit
- Controller validation: read 8 bytes before each order to detect stale pointer
- `cleanup()` method to free buffer on detach

### Live test results (2026-03-24)

- Hero: Lion, Team: Radiant
- **90s at 7Hz — 0 crashes, 0 errors**
- Lua modes producing actions: `push_top`, `push_bot`, `farm` (desire 1.10)
- Ability level-up working: lv2→3 during test
- Entity cache: 9 heroes, 5-11 creeps, 24 towers
- Bot navigates full top lane: fountain → top → enemy creeps → back
- HP dynamics: takes damage from enemies (185 HP), respawns, regains
- Nearby detection works: "2eC 2aC 2eH" when at creep wave

### Remaining issues
- **Roam Think still no-op** — all conditions fail (no ganking targets? missing API?)
- **No spell casting in combat** — AbilityUsageThink returns no action (needs enemies nearby + leveled abilities)
- **Gold/items** — not reading gold, no item purchases

## 2026-03-24 — Phase 7: Critical Fixes (attack, abilities, entity validation)

### 1. Bot doesn't attack — _make_unit_handle only searched heroes (CRITICAL P0)

**Problem:** `_make_unit_handle.get_data()` only searched `brain.cache.heroes`. For creep/tower
handles returned by `GetNearbyCreeps`/`GetNearbyTowers`:
- `GetTeam()` → always returned `brain._our_team` (enemy creeps looked friendly!)
- `GetLocation()` → returned (0,0,128) (no position data)
- `GetHealth()` → returned 1
- `IsBuilding()` → returned False for towers, True was hardcoded elsewhere

Farm mode checked `farmTarget:GetTeam() ~= bot:GetTeam()` → always False → no attack.

**Fix:** `get_data()` now searches ALL caches: heroes → creeps → towers → buildings.
Added `_unit_type()` helper for dynamic type detection from entity name.
- `IsBuilding` now returns True for towers/buildings, False for heroes/creeps
- `IsHero`, `IsTower`, `IsAncient`, `IsFort` all dynamically check entity name
- Added `IsAncientCreep`, `IsCreep` methods
- Implemented `GetNearbyBarracks` (was empty stub, push mode needs it)

**Result:** 147 AttackUnit + 17 AttackMove per 90s (was 0 before fix).

### 2. Slot 2-3 abilities unresolved — garbage entity pointers in handle_index (CRITICAL P1)

**Problem:** Entity indices for abilities (e.g. 1291 = lion_mana_drain) live in sentinel
chunks (chunk 2, which is sentinel in all 3 chunk lists). The Phase A chunk scan found
stale/garbage entries in non-sentinel chunks that happened to have matching handle low bits.

Example: chunk 1 slot 452 had handle=0x50B (idx=1291) but entity_ptr=0x51300000509 — a
garbage pointer that's actually a packed handle value, not a heap address.

The correct entity pointer (0x2308bdd9300) was found by Phase B identity region scan
but `handle_index[1291]` was already populated with garbage from Phase A.

**Fix:** Added entity pointer alignment validation: `(ent_ptr & 0xFF) != 0` → skip.
Dota entity objects are always 0x100-aligned on heap. Garbage pointers like 0x51300000509
fail this check. Applied to both `EntityCache.update()` Phase A and `resolve_handle()`
medium path.

**Result:** handle_index shrunk from 3914 → 2137 entries (garbage eliminated).
All 4 basic abilities + ult now resolve correctly.

### 3. Slot 4 garbage name — module-space entity pointer

**Problem:** Slot 4 (`generic_hidden` placeholder) resolved to entity_ptr=0x7FF93EF759C0,
which is in client.dll code space. `read_ability_data` read random bytes from code as
ability level (1629165184) and designer name (disassembly text).

**Fix:** Added filter in `read_ability_data()`: reject ability_entity > 0x7FF000000000
(module space pointers are not heap entities).

### 4. Missing Lua constants — generic_hidden, DOTA_ABILITY_BEHAVIOR_NOT_LEARNABLE

**Problem:** `generic_hidden` was nil in Lua env. `GetAbilityList` uses `name == generic_hidden`
to skip placeholder abilities. With nil, comparison never matched → wrong ability list.
Also `DOTA_ABILITY_BEHAVIOR_NOT_LEARNABLE` was nil, breaking aba_skill.lua fallback logic.

**Fix:** Added both to `_register_constants()`:
- `generic_hidden = "generic_hidden"` (string constant)
- `DOTA_ABILITY_BEHAVIOR_NOT_LEARNABLE = 64` (same as ABILITY_BEHAVIOR_HIDDEN)

### Live test results (2026-03-24, post-Phase 7)

- **90s at 7Hz — 0 crashes, 0 errors, 322 ticks**
- **147 AttackUnit, 17 AttackMove, 146 Move** — bot actively attacks creeps
- All 5 main abilities resolve: impale, voodoo, mana_drain, to_hell_and_back, finger_of_death
- Bot pushes bot lane: fountain → enemy creep wave → engage → retreat → repeat
- Takes combat damage (770/780 HP when near enemy creeps + hero)
- Push_top mode produces actions, bot follows lane front

## 2026-03-24 — Phase 8: Ability Casting Debug + Offset Fixes

### Bugs found & fixed

#### 1. BaseAbility offsets +8 shift (CRITICAL)
All C_DOTABaseAbility schema offsets were **wrong** — needed same +8 shift as BaseEntity.
Abilities always read as lv=0, making IsFullyCastable() always False.

| Field | Old (schema) | New (verified) |
|-------|-------------|----------------|
| m_iLevel | 0x628 | **0x630** |
| m_fCooldown | 0x638 | **0x640** |
| m_flCooldownLength | 0x63C | **0x644** |
| m_iManaCost | 0x640 | **0x648** |
| m_bHidden | 0x617 | **0x61F** |
| m_bActivated | 0x619 | **0x621** |
| m_bToggleState | 0x62D | **0x635** |
| m_bAutoCastState | 0x644 | **0x64C** |
| m_nAbilityCurrentCharges | 0x660 | **0x668** |
| m_fAbilityChargeRestoreTimeRemaining | 0x664 | **0x66C** |

Verified: lion lv8 → impale=4, voodoo=2, mana_drain=1, innate=1. mana_cost=150/140 correct.

#### 2. Stale ability handles (CRITICAL)
`hero_lion.lua` line 178: `local abilityQ = bot:GetAbilityByName(...)` called at MODULE LOAD TIME.
`_make_ability_handle` captured `level = ab_data.get("level", 0)` as closure variable.
IsFullyCastable() used stale `level` → always 0 even after level-up.

**Fix**: All dynamic fields (`_get_level()`, `_get_cd_remaining()`, `_is_hidden()`, `_get_mana_cost()`)
now re-read from memory via entity pointer on each call. No more stale snapshots.

#### 3. PrepareUnitOrders RVA shifted (CRASH)
11MB Dota patch shifted RVA: 0x1D16120 → **0x1D162E0** (+448 bytes).
Old address was middle of a CALL instruction → crash on any move/attack order.
AOB: `4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08`

#### 4. FindAoELocation missing on unit handle
`hero_lion.lua:348` calls `bot:FindAoELocation(...)` — was only defined as global, not on handle.
**Fix**: Added `handle.FindAoELocation` with real AoE counting logic.

#### 5. GetTarget/GetAttackTarget always nil
`J.GetProperTarget(bot)` calls `bot:GetTarget()` → `brain._target` = None always.
Lua considers `botTarget=nil` → skips all "going on someone" cast logic.
**Fix**: Dynamic `_get_target()` returns nearest enemy hero within 1600 range.

#### 6. GetPlayerID always 0 → IsSuspiciousIllusion false positives
All unit handles returned playerID=0. `J.IsSuspiciousIllusion` does:
`GetHeroLevel(tID) > npcTarget:GetLevel()` — with tID=0 for all, comparing wrong levels.
Could mark real heroes as illusions → filtered from GetNearbyHeroes results.
**Fix**: `GetPlayerID` returns hero's index in cache. `GetHeroLevel(pid)` returns that hero's level.

### BotTrace system added
New `BotTrace` class in bot_brain.py — per-tick event tracing, enabled via `BotBrain(..., trace=True)`.
12 trace points: State, ModeEval, ModeThink, Fallback, Order, RefreshAbilities,
MakeAbilityHandle, IsFullyCastable, FindAbility, CastNT/Ent/Pos, AbilityThink, LuaDiag.
Lua-side diagnostic: injects `J.CanNotUseAbility`, `J.GetNearbyHeroes`, mode, target check
directly from Lua env before AbilityUsageThink call.

### Current status (end of Phase 8)

**What works:**
- Ability offsets correct — levels, cooldowns, mana costs read properly
- Dynamic ability handles — level updates in real-time after level-up
- IsFullyCastable returns True when ability is actually castable
- Bot moves to lane, attacks creeps/heroes
- LevelUp works (AbilityLevelUpThink qualifies skills)
- No crashes (PrepareUnitOrders RVA fixed)
- LuaDiag confirms: `enemies1600=#1 target=true laning=true canNotUse=false`

**Still broken: abilities don't cast**
Even with all fixes, AbilityUsageThink returns without casting. Last LuaDiag showed:
`canNotUse=false invis=false mode=1 target=true enemies1600=#1 enemies1200=#1 laning=true mp=100%`
All preconditions met, but ConsiderE/R/W/Q return 0 desire.

## 2026-03-24 — Phase 9: Algorithm Logging + Ability Casting Fix

### Root Cause Found: mode_attack blocks during laning phase

**Problem chain:**
1. `mode_attack_generic.lua` → `GetDesireBasedOnHp()` line 158: returns 0 during `J.IsInLaningPhase()` (DotaTime < 12min AND NetWorth < 5000)
2. Attack mode desire = 0 → `_active_mode` stays `BOT_MODE_LANING` (1)
3. `ConsiderQ` scenario [4] (Aggression) requires `J.IsGoingOnSomeone(bot)` which checks `bot:GetActiveMode() == BOT_MODE_ATTACK` → false
4. No other ConsiderQ scenario matches early game: [1] Kill needs low HP target, [2] AoE needs 3+ enemies, [3] Teamfight not active, [5-9] special cases, [10] General needs heroLevel >= 15

### 1. BotAlgoLog — structured per-tick logging

New class in `bot_brain.py` (after BotTrace/BotLogger):
- JSONL output to `C:/temp/bot_trace.jsonl` for post-mortem analysis
- Console one-liner per tick: `[T42] | MODE: attack=0.75 laning=0.30 | ConsiderQ=0.75(Q-aggression) | CAST: impale`
- Categories: MODE (desires), ABILITY (ConsiderX results), CAST (orders), DIAG (deep diagnostic), OVERRIDE (mode patches)
- `BotBrain(..., algo_log=True)` to enable

### 2. Lua monkey-patching ConsiderQ/W/E/R

`_inject_algo_trace()` called after `_load_scripts()`:
- Registers `_TRACE(cat, name, desire, motive)` and `_TRACE_EVENT(cat, msg)` Python callbacks in Lua `_G`
- Wraps `BotBuild.ConsiderQ/W/E/R` with pcall + logging (BotBuild is global after Phase 6 fix)
- Wraps `BotBuild.SkillsComplement` to log pre-call state: canNotUse, isInvis, mode, goingOn, mp%, hp%, target, enemies, laningPhase

### 3. Deep LuaDiag upgraded

Pre-AbilityUsageThink Lua eval now checks:
- All original fields (canNotUse, invis, mode, target, enemies)
- `J.IsGoingOnSomeone(bot)` — the key check
- `J.IsInLaningPhase()` — confirms laning phase blocking
- Q-specific: castRange, inRange enemies, target CanCastOnNonMagicImmune, target IsInRange

### 4. Laning→Attack mode override (P0 FIX)

Before `_run_ability_think()`, if:
- `_active_mode == BOT_MODE_LANING`
- Enemy hero within 1600 range (from entity cache)
- Our HP > 30%

Temporarily set `_active_mode = BOT_MODE_ATTACK` + desire=0.75 for duration of AbilityUsageThink.
Restored after. This makes `IsGoingOnSomeone(bot)` return true → ConsiderQ aggression path fires.

### Files modified
- `cheat/bot_brain.py` — BotAlgoLog class, _inject_algo_trace(), mode override in think_once(), algo_log.tick_done() in run()
- `/c/temp/trace_bot.py` — added algo_log=True, updated console messages
- `/c/temp/run_bot.py` — added algo_log=True

### Console output format
```
  [T1] | MODE: roam=0.70 laning=0.50 attack=0.00 [->laning->attack] | ConsiderQ=0.75(Q-aggression) ConsiderW=0 | CAST: impale@(-500,200) | hp=680/780 mp=400
```

### 5. Python cast fallback — `_python_cast_fallback()` (VERIFIED WORKING)

Lua `SkillsComplement` не вызывается из sandbox (Lua 5.4 env scoping issue).
Вместо дальнейшего дебага sandbox — прямой Python fallback через PrepareUnitOrders.

**Как работает:**
- Каждый тик, если Lua не скастовала — Python пробует скастовать ВСЕ готовые абилки (Q→W→E→R)
- Для каждой ability: читает level, cd, mana_cost, hidden напрямую из памяти
- Cast type discovery: пробует `cast_target` → `cast_no_target` → `cast_position`, проверяет мана
- Результат кешируется в `_ab_cast_type[slot]` — discovery только при первом касте

**Skywrath Mage verified:**
| Slot | Ability | Cast type | Verified |
|------|---------|-----------|----------|
| 0 | Arcane Bolt | `cast_target` | spent 68 mana ✓ |
| 1 | Concussive Shot | `cast_no_target` | spent 87 mana ✓ |
| 2 | Ancient Seal | `cast_target` | spent 106 mana ✓ |
| 5 | Mystic Flare | `cast_position` | spent 788 mana ✓ |

**Bug fixed:** `hp > 1000` фильтр из AC отсекал Dota-героев (HP 2000+). Заменён на `hp > 50000`.

**Live test:** T1 = `PY_CAST(x4)` (все 4 абилки, mana 1959→894), затем Arcane Bolt спамится каждый тик.

### Нерешённые проблемы

**Lua brain НЕ работает:**
- `AbilityUsageThink` вызывается, guard clauses проходят, но `BotBuild.SkillsComplement()` не вызывается из sandbox env (Lua 5.4 `_ENV` scoping — sandbox reads `BotBuild` from `_G` через `__index`, но wrapped функция не видна при вызове из sandbox chunk)
- `ConsiderQ/W/E/R` всегда возвращают 0 — потому что SkillsComplement не вызывается
- Replaced AbilityUsageThink тоже не помогает — та же sandbox проблема
- **Вывод:** Lua sandbox `_sandbox_load` несовместим с monkey-patching. Нужен другой подход к integration.

**Бот бегает бессмысленно:**
- `mode_attack_generic` блокирован в laning phase (GetDesireBasedOnHp returns 0)
- `mode_laning_generic` не имеет Think() — только GetDesire
- `roam` mode Think() not producing actions
- `push_top` mode с desire 0.02 выигрывает и гонит бота на top lane
- Fallback AI (`_fallback_think`) слишком примитивный — walk to lane + last hit
- Бот не преследует врагов, не отступает осмысленно, не выбирает lane

## 2026-03-24 — Phase 10: Python Brain (replacing Lua brain)

### Motivation
Lua brain мёртв: `SkillsComplement` не вызывается из sandbox (Lua 5.4 `_ENV` scoping),
`ConsiderQ/W/E/R` всегда 0, mode Think() большей частью no-op.
Стратегия: **полностью заменить Lua think на Python decision tree** поверх рабочих примитивов.

### Architecture

`think_once()` переписан:
1. `_evaluate_state()` — собирает всю информацию за 1 тик:
   - HP/mana/level, позиция
   - Враги-герои (sorted by dist), ближайший < 1600
   - Союзники-герои nearby
   - Вражеские/союзные крипы (nearby < 900)
   - Вражеские/союзные башни, `under_enemy_tower` (< 700)
   - Дистанция до фонтана
   - Игровое время

2. `_python_brain_think(state)` — decision tree:
   - **P0 DEAD**: не делать ничего
   - **P0 SURVIVAL**: HP < 30% → retreat к башне/фонтану
   - **P0 TOWER_DANGER**: под вражеской башней без крипов → retreat
   - **P0 COMBAT**: враг-герой < 1600 → `_combat_think()`
   - **P1 LANING**: крипы nearby → `_laning_think()` (last hit, deny, hold wave)
   - **P1 ADVANCE**: идти по лейну → `_advance_think()`

3. `_combat_think(state, enemy)`:
   - Don't chase under enemy tower (без крипов)
   - ALL_IN: враг < 25% HP, мы > 40% → attack-move
   - AGGRO: наше HP% > вражеское + 15% или есть союзники → attack
   - HARASS: держать дистанцию ~800, attack-move
   - KITE: враг < 400 и мы < 50% HP → отступить

4. `_laning_think(state)`:
   - LH: вражеский крип с HP < damage * 1.3
   - DENY: союзный крип с HP < damage * 1.3
   - HOLD_WAVE: стоять чуть позади союзных крипов

5. `_advance_think(state)`:
   - Идти по waypoints, не заходить под вражескую башню

6. После movement: `_python_cast_fallback()` (Q→W→E→R) + Lua `AbilityLevelUpThink`

### What changed
- `think_once()` — полностью переписан, Lua Think/modes отключены
- `_evaluate_state()` — новый метод, все данные за 1 тик
- `_python_brain_think()` — decision tree
- `_combat_think()`, `_laning_think()`, `_advance_think()` — новые
- `_pos_under_enemy_tower()`, `_nearest_safe_pos()` — helpers
- `_fallback_think()` — тонкая обёртка для обратной совместимости
- `/c/temp/run_bot.py` — обновлён (waits for game start, 90s default)
- `/c/temp/trace_bot.py` — обновлён

### Preserved from Phase 9
- `_python_cast_fallback()` — verified working, не трогали
- `BotAlgoLog` — JSONL tracing
- `EntityCache` — entity scanning
- `_run_ability_think()` → используется только для LevelUp
- Lua scripts всё ещё загружаются (для LevelUp + API reference)

### Iteration 2: Fixes after first test (same session)

**Problem 1: Only slot1 (W) casting, detected as `position` (wrong)**
- Fix: increased cast type discovery sleep 150ms→300ms + added verification for position type
- Now properly discovers `no_target` for Concussive Shot
- Added `unknown` type — skips ability if all 3 types fail

**Problem 2: Zero last hits — HOLD_WAVE 100% of time**
- Root cause: LH threshold too narrow (dmg*1.3=53 HP), using `attack_move` not `attack_target`
- Fix: `_execute_attack_target(entity_ptr)` — resolves eidx via identity, uses ATTACK_TARGET order
- LH threshold raised to dmg*1.5
- Added `LH_APPROACH` — moves towards low-HP creeps (< 250 HP) before they're in kill range
- Hold position 100 units behind wave (was 200 — too far back)

**Problem 3: Combat priority too high — never farms**
- Old: enemy hero < 1200 → always combat → never laning
- Fix: combat only triggers at < 600 range or enemy killable (< 25% HP)
- Laning now has priority over distant hero harass
- Result: bot farms creeps AND harasses when opportunity

**Problem 4: Late retreat (HP 409→148 in 5 ticks)**
- Fix: retreat threshold 30%→35%

### Test results (QoP, 90s run, 261 ticks)
| Action | Count |
|--------|-------|
| HOLD_WAVE | 175 |
| HARASS | 60 |
| **LH** | **12** |
| **DENY** | **4** |
| KITE_TOWER | 8 |
| KITE | 2 |
| Casts (slot2=Scream) | 5 |

Brain decision tree verified: LH, deny, retreat, tower awareness, kiting all functional.

## 2026-03-24 — Phase 11: TP Logic + Cast Pre-Cache + LH Improvements + Item Purchasing

### 1. Cast Type Pre-Cache from game_data (P1 — replaces 300ms probe)

**Problem:** Cast type discovery required 300ms sleep per ability type (up to 900ms for 3 tries).
Also slot0 (Shadow Strike on QoP) failed discovery entirely.

**Solution:** `GameDataDB.ability_cast_type(name)` reads `AbilityBehavior` from JSON:
- `DOTA_ABILITY_BEHAVIOR_UNIT_TARGET` → `"target"`
- `DOTA_ABILITY_BEHAVIOR_POINT` → `"position"`
- `DOTA_ABILITY_BEHAVIOR_NO_TARGET` → `"no_target"`
- `DOTA_ABILITY_BEHAVIOR_TOGGLE` → `"no_target"`
- Passive/Hidden → `None` (skip)

New `_precache_cast_types()` method in `BotBrain`:
- Called once on first `_python_cast_fallback()` invocation
- Reads ability names from memory (identity → designer_name)
- Looks up cast type from game_data JSON
- Populates `_ab_cast_type[slot]` instantly (no probing)
- Fallback: mana probe still available for abilities not in JSON

**Impact:** Zero-latency ability casting on first encounter. No more 300ms+ stalls.

### 2. TP Scroll Logic (P0)

**New methods in BotBrain:**
- `_find_tp_in_inventory()` → reads item handles from ITEMS_INLINE, resolves names, returns (entity_index, slot) for TP scroll
- `_buy_tp()` → `cmd.execute("dota_purchase_item item_tpscroll")`
- `_tp_to_position(x, y, z)` → `cmd.cast_position(tp_eidx, x, y, z)`
- `_tp_to_nearest_tower(state)` → finds closest ally tower to mid lane, TPs there
- `_tp_home(state)` → TPs to fountain

**Integration in decision tree:**
- After respawn (at fountain, HP > 90%): TP to nearest ally tower (rate limit 10s)
- If no TP in inventory: buy one (rate limit 15s)
- When HP < 15% and dist_fountain > 4000: TP home
- While advancing: ensure TP in inventory (check every 30s)

### 3. Improved Last Hit Logic (P1)

**Changes to `_laning_think()`:**
- **Ranged/melee detection**: `is_ranged = attack_range > 200`
- **Tighter LH threshold for ranged** (1.2x damage vs 1.5x for melee) — accounts for projectile delay
- **Wider approach radius**: 1200 → 1500 units, HP threshold 250 → 350 (position earlier)
- **Ranged approach**: get 150 units closer than max range for reliable hit
- **Hold position**: ranged heroes stand 150 units behind wave (vs 50 for melee)
- **Deny eligibility**: only attempt deny when creep HP < 275 (50% of melee creep HP)

### 4. Gold Reading (P0 — partial)

**Approach:** Three-tier gold reading:
1. DataNonSpectator entity (reliable + unreliable) — primary, but doesn't work in all cases
2. Hero entity + 0xAC0 (BaseNPC.GOLD) — direct read, may be passive income ticks
3. Fallback: 600 (starting gold)

**Note:** 0xAC0 is unreliable but gives non-zero values in live matches, which is
enough for basic purchasing decisions. Full gold system needs DataNonSpectator reversal.

### 5. Basic Item Purchasing (P2 — VERIFIED WORKING)

Simple item build order in `_try_buy_items()`:
- `item_tango` → `item_branches` x2 → `item_magic_stick` → `item_boots` → `item_magic_wand` → `item_wind_lace` → `item_ring_of_basilius`
- Sends `dota_purchase_item` unconditionally — Dota rejects if insufficient gold
- Rate limited to 1 item per 8s, sequential through build order
- Scans inventory to skip items already owned (non-stackable)

**CRITICAL FINDINGS:**

1. **`dota_purchase_item` console command does NOT work** — not a valid ConCommand.
   Only exists as ConVars like `dota_purchase_quickbuy` (needs items in quickbuy first).

2. **PrepareUnitOrders PURCHASE_ITEM (order_type=16) WORKS!**
   - `ability_index = item_definition_ID` (from items.json "ID" field)
   - e.g. tango=44, branches=16, boots=29, clarity=38, magic_stick=34
   - Works from ANYWHERE — items go to stash if not at shop
   - Dota rejects silently if insufficient gold
   - Verified: tango, branches, wind_lace(244), magic_stick(34) all purchased ✓

3. **CEntityIdentity flat stride = 0x70 (NOT 0x78!)**
   - Flat index: `list_base + entity_index * 0x70`
   - Handle at identity+0x10 (lower 32 bits), entity_ptr at +0x00, designer_name at +0x18
   - Chunk-based iteration still uses stride 0x78 and designer_name at +0x20
   - Both coexist: chunks for entity discovery, flat for handle resolution
   - `resolve_handle()` now uses FLAT_STRIDE=0x70 for medium path

4. **Item definition IDs** added to `DotaCommands.ITEM_DEF_IDS` dict (~70 common items).
   Fallback: load from GameDataDB JSON if not in dict.

### Live test results (90s, QoP, Phase 11 final)

| Action | Count/Detail |
|--------|-------------|
| **Item purchases** | 8/8: tango, branches×2, magic_stick, boots, magic_wand, wind_lace, basilius |
| **TP from fountain** | 3× (respawn → TP to lane) |
| **TP buy** | 2× (auto-buy when no TP) |
| **Cast pre-cache** | {0:'no_target', 1:'position', 3:'target', 5:'target'} — instant |
| **Ability casts** | PyCast:slot0 ×10 |
| **Retreat** | HP 26%→11% → retreat to tower |
| **Combat** | Engaged enemies, took damage, retreated |

### Phase 11 — COMPLETE ✓

### Files Modified
```
cheat/
  game_data.py   — +ability_cast_type() method, +BEHAVIOR_* constants
  bot_brain.py   — +_precache_cast_types(), +TP methods (5), +_try_buy_items(),
                   +_ITEM_BUILD, improved _laning_think(), improved _get_gold(),
                   improved _python_brain_think() (TP integration)
```

---

## C++ Migration — Phase 3: Commands (2026-03-26)

### Цель

Заменить Python shellcode (40+ строк VirtualAllocEx + CreateRemoteThread) на прямые вызовы из internal DLL. Два модуля: CConsoleCmd (console commands) и CPrepareOrders (unit orders).

### Реализовано

**CConsoleCmd** — обёртка над `CCommandBuffer::AddText`:
- `CCommandBuffer[0]` (client slot): hardcoded RVA `engine2.dll + 0x8CF9E8` (данные в .data, не код — AOB скан невозможен)
- `AddText`: hardcoded RVA `tier0.dll + 0x5BC60`
- Verify: `CmdBuffer + 0x8028 == 0x400` (max command length) — проходит
- Сигнатура: `void __fastcall AddText(CCommandBuffer*, const char* cmd, int delay, int unk, bool, bool, uint64_t flags)`
- Один вызов: `GetConsoleCmd()->Execute("echo Hello")`

**CPrepareOrders** — обёртка над `PrepareUnitOrders`:
- AOB: `4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08` (из Python, verified)
- Добавлен в CFunctionList как CBasePattern (SEARCH_TYPE_NONE)
- Сигнатура: `void __fastcall PrepareUnitOrders(controller, orderType, targetIdx, vec3*, abilityIdx, issuer, unit, queue, showEffects)`
- 8 convenience wrappers: MoveTo, AttackMove, AttackTarget, Stop, CastPosition, CastTarget, CastNoTarget, TrainAbility
- Enums: EDOTAUnitOrder (17 значений), EOrderIssuer (4 значения)

**CConVarSystem** — отложен (не нужен для Phase 5 StateMachine).

### Hot-reload (END hotkey)

Добавлен хоткей END для выгрузки DLL без перезапуска Dota:
- `MH_DisableHook(MH_ALL_HOOKS)` — отключает все MinHook хуки (Present, OnCreateMove, OnAddEntity/Remove)
- `SetWindowLongPtrA(original)` — восстанавливает WndProc
- ImGui/DX ресурсы НЕ освобождаются (leak ~2MB) — предотвращает use-after-free краш
- Первая версия (полный OnDestroy + DestroyHooks) крашила — ImGui::DestroyContext() вызывался до снятия Present hook

### Live test (2026-03-26, Dota bot match)

```
[+] CBasePattern: PrepareUnitOrders -> 00007FF9C73C72A0
[CConsoleCmd] Init OK: buf=0x00007FF9DCCDF9E8 addtext=0x00007FF9DCDDBC60
[PrepareOrders] Init OK: fn=0x00007FF9C73C72A0
```

- ✅ PrepareUnitOrders AOB scan — нашёл с первого раза
- ✅ CConsoleCmd verify (+0x8028 == 0x400) — прошёл
- ✅ AddText sanity (first byte != 0x00/0xCC) — прошёл
- ✅ Overlay: 4 строки, 4-я (magenta): "Cmd: OK | Orders: OK"
- ✅ END hotkey: overlay пропадает, игра работает, можно реинжектить

### Файлы (8 изменений)

| Файл | Действие |
|------|----------|
| `BotFarm/Commands/CConsoleCmd.hpp` | NEW — Execute(const char*), IsReady() |
| `BotFarm/Commands/CConsoleCmd.cpp` | NEW — hardcoded RVA + verify + AddText call |
| `BotFarm/Commands/CPrepareOrders.hpp` | NEW — EDOTAUnitOrder enum, 8 wrappers |
| `BotFarm/Commands/CPrepareOrders.cpp` | NEW — CFunctionList pattern + SendOrder impl |
| `Dota2/SDK/CFunctionList.hpp` | +PrepareUnitOrders CBasePattern |
| `Dota2/SDK/CFunctionList.cpp` | +&PrepareUnitOrders в vPatterns |
| `BotFarm/CBotFarmClient.cpp` | +Commands init + 4-я overlay строка |
| `Andromeda-Dota2-Base.vcxproj` | +4 файла (2×ClCompile + 2×ClInclude) |
| `AndromedaClient/CAndromedaGUI.cpp` | +END hotkey (MH_DisableHook + WndProc restore) |

### Phase 3 — COMPLETE ✓

---

## Phase 6: Game Data + Ability/Item System (2026-03-27)

### Цель
Загрузка статических game data (герои, абилки, предметы) из JSON + чтение live ability/item данных из памяти через schema offsets.

### Реализация

#### 1. nlohmann/json v3.11.3
- Скачан single-header (24765 строк) в `Deps/json/json.hpp`
- JSON файлы скопированы в `C:\temp\andromeda\data\`: npc_heroes.json (1MB), npc_abilities.json (1.4MB), items.json (320KB)

#### 2. CGameDataDB (BotFarm/Data/)
Порт Python `GameDataDB` на C++. Singleton, загрузка при Init():
- 128 героев (merge с npc_dota_hero_base template)
- 1898 абилок (level-aware parsing: "21 18 15 12" → vector<float>)
- 560 предметов (cost, cooldown, secret shop flag)
- AbilityValues (nested dict) — полный парсинг
- 20 behavior flags, `GetAbilityCastType()` → "target"/"position"/"no_target"

#### 3. Schema offsets добавлены

**C_DOTABaseAbility** (10 полей):
`m_iLevel, m_fCooldown, m_flCooldownLength, m_iManaCost, m_bActivated, m_nAbilityCurrentCharges, m_bInAbilityPhase, m_bToggleState, m_bAutoCastState, m_iMaxLevel`

**C_DOTA_Item** (3 поля):
`m_iCurrentCharges, m_bItemEnabled, m_iPlayerOwnerID`

**C_DOTA_BaseNPC** (+2):
`m_vecAbilities` (CNetworkUtlVectorBase<CHandle>), `m_Inventory` (PSCHEMA_OFFSET → C_DOTA_UnitInventory*)

**C_DOTA_UnitInventory**:
`m_hItems` (CNetworkUtlVectorInline<CHandle>) — INLINE data

#### 4. EntityCache расширен
- `AbilityData` struct: name, slot, level, cooldown, cooldownLength, manaCost, charges, isActivated, isInPhase, isToggled
- `ItemData` struct: name, slot, level, cooldown, cooldownLength, charges, isEnabled
- `RefreshAbilities()` — итерация m_vecAbilities, resolve CHandle → C_DOTABaseAbility
- `RefreshItems()` — m_Inventory → m_hItems (inline vector), skip INVALID handles
- `FindAbility(name)`, `FindItem(name)`, `GetLearnedAbilityCount()` — helper methods

### CRITICAL: Два типа network vector в Source 2

Обнаружено при отладке: `C_NetworkUtlVectorBase<T>` имеет ДВА разных memory layout:

**Pointer-based** (`CNetworkUtlVectorBase<T>`):
```
+0x00: int32 m_Size
+0x04: char pad[4]
+0x08: T* m_pData    ← указатель на heap allocation
```
Используется: `m_vecAbilities` (count=21, pData=valid heap pointer)

**Inline** (`CNetworkUtlVectorInline<T>`):
```
+0x00: int32 m_Size
+0x04: T m_Data[]    ← данные СРАЗУ после count, без указателя
```
Используется: `m_hItems` (count=25 = все inventory слоты, CHandle[] inline)

**Как обнаружено**: pymem hex dump inventory+0x20 показал count=25, а следующие 8 байт = 0xFFFFFFFFFFFFFFFF. Это ДВА INVALID CHandle (0xFFFFFFFF каждый), прочитанных как один 64-bit pointer. Подтверждено покупкой предмета — handle[0] стал валидным (0x0148016C).

### Баги найденные

1. **`%zu` в DEV_LOG** — XorStr оборачивает format string, vsnprintf с `%zu` через XorStr молча ничего не пишет. Решение: `%d` + `(int)` каст.

2. **debug.log path** — `GetDllDir()` при BlackBone manual map возвращает CWD dota2.exe = `C:\Users\aleks\OneDrive\Документы\реверс курс\`. Актуальный лог: `реверс курс\debug.log`, НЕ `C:\temp\andromeda\debug.log`.

### Тест (verified live, бот-матч, 2026-03-27)

```
[GameDataDB] Loaded: 128 heroes, 1898 abilities, 560 items
[EntityCache] Abilities: npc=... count=21 pData=... (valid)
```
- ✅ JSON загрузка: 128H/1898A/560I
- ✅ Abilities: 21 слот, level/cooldown/manaCost читаются
- ✅ Items: inline vector, покупаемые предметы видны в overlay
- ✅ Overlay line 7: `Data: 128H 1898A 560I | Q(1) W(0) E(0) R(0) [2 items]`

### Файлы (9 изменений)

| Файл | Действие |
|------|----------|
| `Deps/json/json.hpp` | NEW — nlohmann/json v3.11.3 |
| `BotFarm/Data/CGameDataDB.hpp` | NEW — structs + API |
| `BotFarm/Data/CGameDataDB.cpp` | NEW — JSON loading + lookups |
| `Dota2/SDK/Types/CEntityData.hpp` | +CNetworkUtlVectorBase, +CNetworkUtlVectorInline, +C_DOTABaseAbility(10), +C_DOTA_Item(3), +C_DOTA_UnitInventory, +m_vecAbilities/m_Inventory |
| `BotFarm/GameState/CEntityCache.hpp` | +AbilityData, +ItemData, +FindAbility/FindItem |
| `BotFarm/GameState/CEntityCache.cpp` | +RefreshAbilities(), +RefreshItems() |
| `BotFarm/CBotFarmClient.cpp` | +GameDataDB init, +overlay line 7, fix %zu |
| `BotFarm/Settings/Settings.hpp` | +szDataDir |
| `Andromeda-Dota2-Base.vcxproj` | +CGameDataDB.cpp/.hpp, +json.hpp |

---

## Phase 7: Bot Brain — Sub-session 1+2 (2026-03-28)

### Цель
Встроить Lua runtime (sol2 + Lua 5.4.7) в DLL для загрузки OpenHyperAI bot скриптов. C++ fallback brain пока скриптов нет.

### Зависимости добавлены
- **Lua 5.4.7** — скомпилирован как static lib `lua54.lib` (x64, /MT, LUA_COMPAT_5_3). Headers: `Deps/lua54/`
- **sol2 v3.3.1** — header-only, `Deps/sol2/sol/` (109 файлов). Include path: `Deps/sol2/` → `<sol/sol.hpp>`
- DLL размер: 10.3MB → 10.6MB (+0.3MB от sol2/Lua templates)

### Sub-session 1: CBotBrain scaffold + Vector + Constants

**CBotBrain** — Init/Think/Shutdown lifecycle:
- `Init()`: создаёт `sol::state`, регистрирует Vector/Constants/Proxies/Globals, находит local hero через EntityCache
- `Think()`: C++ fallback brain (retreat/attack/lane), вызывается из OnRender через StateMachine 10Hz
- `Shutdown()`: `delete sol::state`, reset state. Вызывается в POST_GAME и disconnect

**LuaVector** — `Vector3` как sol2 usertype `"Vector"`:
- ctor(x,y,z), поля x/y/z, арифметика (+/-/*/÷/unary-/==)
- Methods: Length, Length2D, Normalized, Dot, Cross, Distance, Distance2D
- Привязан к существующему SDK `Vector3` (не отдельный тип)

**LuaConstants** — ~60 констант в Lua globals:
- BOT_MODE_* (22), LANE_* (4), UNIT_LIST_* (8), DAMAGE_TYPE_* (4)
- ABILITY_BEHAVIOR_* (12), TEAM_*, GAME_STATE_*, RUNE_*, PURCHASE_ITEM_*
- BOT_ACTION_TYPE_*, BOT_MODE_DESIRE_*, DIFFICULTY_*, ITEM_SLOT_TYPE_*

**Settings**: +`bUseLuaBrain(true)`, +`szScriptsDir("C:\temp\andromeda\scripts")`

**Integration**: StateMachine GAME_IN_PROGRESS → `GetBotBrain()->Init()`, OnTick → `Think()`, POST_GAME → `Shutdown()`. Lazy init в OnTick для hot-inject.

**Overlay line 8** (lime green): `"BotBrain: LANE king L2 134/648"`

### Sub-session 2: Entity Proxies + Actions + PanoramaJS

**LuaUnitProxy** — три sol2 usertypes:
- `UnitHandle(entityIndex)` — ~30 methods: GetUnitName, GetTeam, GetHealth/MaxHealth/Mana/MaxMana, IsAlive/IsHero/IsCreep/IsTower/IsBuilding, GetLocation, GetLevel, GetAttackDamage, GetAbilityByName/InSlot, GetItemInSlot, GetNearbyHeroes/Creeps/Towers, Action_MoveToLocation/AttackUnit/UseAbility/UseAbilityOnEntity/UseAbilityOnLocation
- `AbilityHandle(entityIndex, slot)` — ~15 methods: GetName, GetLevel, GetCooldownTimeRemaining, GetManaCost, IsFullyCastable, GetBehavior, GetCastRange, GetSpecialValueFor, GetEntIndex
- `ItemHandle(entityIndex, slot)` — ~8 methods: GetName, GetCooldown, GetCurrentCharges, IsFullyCastable, GetCost, GetEntIndex
- Proxy pattern: хранят `entityIndex`, каждый вызов идёт через `GetEntityCache()->GetEntityByIndex()` → always live data, no dangling

**AbilityData/ItemData**: +`entIndex` поле (entity index в GameEntitySystem, нужен для PrepareUnitOrders)

**Lua globals зарегистрированы**:
- `GetBot()` → UnitHandle local hero
- `GetUnitList(type)` → table of UnitHandles (ALLIED/ENEMY HEROES/CREEPS/BUILDINGS)
- `ActionImmediate_PurchaseItem(name)`, `ActionImmediate_LevelAbility(ability)`, `ActionImmediate_Chat`, `ActionImmediate_Buyback`, `ActionImmediate_Glyph`
- `GetItemCost(name)`, `IsItemPurchasedFromSecretShop(name)`, `GetTeam()`, `GetOpposingTeam()`
- `DotaTime()`, `GameTime()`, `RealTime()`, `print()`

### CRITICAL: PrepareUnitOrders НЕ работает из hook threads

**Проблема**: прямой вызов `PrepareUnitOrders` и `CCommandBuffer::AddText` из DLL hook threads (OnRender/OnCreateMove) **молча игнорируется**. Функции возвращают без ошибки, но ордера не исполняются. Это было подтверждено многократным тестированием: позиция не менялась, abilities не качались, items не покупались.

**Причина**: Python бот использовал `CreateRemoteThread` + shellcode для вызова этих функций. Отдельный поток, созданный через Win32 API, корректно обрабатывается движком Source 2. Вызов из hook context (DX11 Present, OnCreateMove) — нет.

**Решение: CPanoramaJS** — вызов `CUIEngine::RunScript` через `CreateThread`:
- `panorama.dll + 0x569C78` → `CUIEngine*` singleton
- `panorama.dll + 0xA6A40` → `RunScript(engine, panel, jsCode, origin, flags)`
- `Execute(jsCode)` создаёт `CreateThread` → thread вызывает `RunScript` → возвращается
- Все ордера через `Game.PrepareUnitOrders({OrderType: N, Position: [...], ...})`

**Wrappers**: `OrderMove`, `OrderAttackTarget`, `OrderAttackMove`, `OrderStop`, `OrderCastNoTarget/Target/Position`, `OrderTrainAbility`, `OrderPurchaseItem`

**Verified live (2026-03-28)**: герой двигается к лейну, атакует крипов, качает ability (hellfire_blast lvl 0→1), position меняется каждый тик.

### Известные баги (для Sub-session 3)
1. **AutoLevel**: innate ability (vampiric_spirit lvl=1) считается в `GetLearnedAbilityCount()` → бот думает нет свободных очков на lvl 2+
2. **Покупка предметов**: `OrderPurchaseItem` через Panorama JS не реализована корректно (нужен Entities.GetAbilityByName для item entity index)
3. **Fallback brain примитивен**: retreat < 25% HP, attack creep < 800, else walk mid. Нет покупки, нет кастования абилок

### Файлы (Sub-session 1+2, 12 изменений)

| Файл | Действие |
|------|----------|
| `Deps/lua54/` | NEW — lua.h, lauxlib.h, lualib.h, luaconf.h |
| `Deps/sol2/sol/` | NEW — sol2 v3.3.1 (109 headers) |
| `lua54.lib` | NEW — compiled static lib (x64 /MT) |
| `BotFarm/BotBrain/CBotBrain.hpp/cpp` | NEW — Init/Think/Shutdown, sol::state, hero tracking, C++ fallback brain |
| `BotFarm/BotBrain/LuaVector.hpp` | NEW — Vector3 sol2 usertype |
| `BotFarm/BotBrain/LuaConstants.hpp` | NEW — ~60 Dota 2 Bot API constants |
| `BotFarm/BotBrain/LuaUnitProxy.hpp/cpp` | NEW — UnitHandle/AbilityHandle/ItemHandle sol2 usertypes |
| `BotFarm/Commands/CPanoramaJS.hpp/cpp` | NEW — CUIEngine::RunScript via CreateThread |
| `BotFarm/GameState/CEntityCache.hpp` | +entIndex in AbilityData/ItemData |
| `BotFarm/GameState/CEntityCache.cpp` | +store hAbility.GetEntryIndex() / hItem.GetEntryIndex() |
| `BotFarm/Settings/Settings.hpp` | +bUseLuaBrain, +szScriptsDir |
| `BotFarm/StateMachine/CBotStateMachine.cpp` | +BotBrain Init/Think/Shutdown integration |
| `BotFarm/CBotFarmClient.cpp` | +overlay line 8, +PanoramaJS include |
| `Andromeda-Dota2-Base.vcxproj` | +include paths (lua54, sol2), +lua54.lib, +SOL_ALL_SAFETIES_ON, +7 source files |

### Engine RVA (обновлено)

| Что | RVA | Модуль | Метод |
|-----|-----|--------|-------|
| CUIEngine* singleton | 0x569C78 | panorama.dll | hardcoded |
| CUIEngine::RunScript | 0xA6A40 | panorama.dll | hardcoded |
| V8 Isolate | CUIEngine+0x600 | panorama.dll | verify check |
| AddText (export) | 0x5BC60 | tier0.dll | GetProcAddress fallback |

### Билд и инжект

```bash
# Билд
/c/temp/build_andromeda.bat

# Копировать DLL к инжектору
cp "C:/temp/andromeda_src/Andromeda-Dota2-Base/x64/Release/Andromeda-Dota2-Base.dll" \
   "C:/temp/andromeda_src/Andromeda-Injector/x64/Release/Andromeda-Dota2-Base.dll"

# Инжект
"C:/temp/andromeda_src/Andromeda-Injector/x64/Release/Andromeda-Dota2-Base.exe"
```

### BotBrain file log
- Путь: `C:\temp\andromeda\botbrain.log` (открыт с `_SH_DENYNO` — можно читать пока DLL работает)
- `cat /c/temp/andromeda/botbrain.log` для диагностики

### Phase 6 — COMPLETE ✓

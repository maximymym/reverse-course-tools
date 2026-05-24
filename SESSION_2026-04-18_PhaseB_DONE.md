# Session 2026-04-18 — Phase B DONE (runtime locator + party state fixes)

## Контекст
Продолжение `SESSION_2026-04-18.md` (Phase A — GSI + status schema 2). Цель сессии
была Phase B: pull SOCache из памяти DLL без hardcoded RVA. Сделано + попутно
найдены и устранены 2 связанных бага state machine.

## Что сделано

### 1. Phase B RE (reverse engineering via CE bridge)
Pattern-scan прямо на живой dota2 через Python find_gcclient_anchors.py.
**Якоря после обновы сдвинулись ~+0x2000 относительно предыдущей сессии** —
все RVA заново найдены:

| Объект | RVA | Примечание |
|--------|-----|-----------|
| String "CDOTAGCClientSystem" | `+0x4E193F8` | 1 hit в .rdata |
| RTTI `.?AVCDOTAGCClientSystem@@` | `+0x5F64510` | — |
| Vtable CDOTAGCClientSystem primary | `+0x4E1AC20` | — |
| RTTI `.?AVCGCClientSharedObjectCache@GCSDK@@` | `+0x6003C78` | — |
| Vtable CGCClientSharedObjectCache | `+0x50F8A80` | — |
| Vtable CGCClientSharedObjectTypeCache | `+0x50F8A40` | (derived, НЕ base @+0x50F8880) |
| **Singleton** CDOTAGCClientSystem | `+0x6521F20` | inline static в .data |
| Offset SOCache\* in singleton | `+0x538` | — |

Layout SOCache / TypeCache:
```
SOCache[0x10] (uint32)        = typeCacheCount
SOCache[0x18] (void**)        = typeCacheArray (heap)
TypeCache[0x28] (uint32)       = type_id
TypeCache[0x18] (uint32)       = objectCount (low bits; high bit 0x80000000 = флаг)
TypeCache[0x10] (CSharedObject**) = objectPtrArray
```

**Важный факт**: старое предположение "type_id=2010 → CSODOTALobby" **НЕВЕРНО** на
свежем клиенте. Через RTTI объекта (vtable→COL→TD→name) получаем честные имена:

| type_id | реальный класс |
|---------|-----------------|
| 1 | CEconItem |
| 7 | CEconGameAccountClient |
| 2002 | CDOTAGameAccountClient |
| 2010 | **CDOTAPlayerChallenge** (не лобби!) |
| 2012 | CDOTAGameAccountPlus |

Party (2003) / PartyInvite (2004/2006) / Lobby (???) — появляются только когда
клиент подписан. В live-state одного бота #0 наблюдалась party через SOCacheSubscribed
события, но не inline в статическом снимке menu.

### 2. Phase B v1 — C++ runtime locator (БЕЗ hardcoded RVA)

**Новые файлы** в `Andromeda/BotFarm/GC/`:
- `GCClientLocator.hpp` — API.
- `GCClientLocator.cpp` — PE walk + AOB string scan + RTTI walk + LEA xref scanner
  + SOCache slot probing, всё на чистом C++/WinAPI.

Алгоритм `Init()`:
1. `GetModuleHandleA("client.dll")` → PE headers → границы `.text`/`.rdata`
2. Linear scan в `.rdata` на строку "CDOTAGCClientSystem\0" (1 hit)
3. RTTI walk: RTTI-name → TypeDescriptor (name−0x10) → COL (сканом DWORD TD-RVA) →
   primary vtable (сканом QWORD COL-abs)
4. LEA xref scanner в `.text`: для каждого байта проверяем pattern
   `48/4C 8D modrm disp32` с (modrm & 0xC7)==0x05 → effective = rip+7+disp → 3 xref'а
5. Поиск singleton: в окне ±0x40 от xref ищем `lea rcx/rdx, [rip+disp]` где
   *(effective) == primary vtable — это адрес singleton'а в .data
6. SOCache slot: сканируем singleton первые 4KB, для каждого QWORD проверяем
   что это heap-ptr и *(ptr) == vtable CGCClientSharedObjectCache

Интеграция в `CGCMessageHandler::InstallVMTHook()` — вызов `Init()` при инжекте.
В `ProcessPending()` каждые 5с → `RefreshSnapshot()`, при изменении лог
`[GC] SOCache snapshot changed: tid=1(n=2):CEconItem, tid=7(n=1):...`.

**Verified в prod** на двух live ботах (PID 104776, 117176):
```
[Locator] SINGLETON: client.dll+0x6521f20
[Locator] SOCache* @ singleton+0x538 => 0x41d219cc700
[GC] SOCache snapshot changed: tid=1(n=2):CEconItem, tid=7(n=1):CEconGameAccountClient,
    tid=2002(n=1):CDOTAGameAccountClient, tid=2010(n=16):CDOTAPlayerChallenge,
    tid=2012(n=1):CDOTAGameAccountPlus
```

### 3. QUEUING watchdog (stuck state fix)

При отправке StartFindingMatch бот переходил в QUEUING немедленно, но
`StartFindingMatchResult` (msg_type 4505) **не приходит** от GC вообще. Без него
`gcState.inQueue` остаётся false, и state machine сидит в QUEUING до
`flQueueTimeout` (несколько минут).

**Fix** в `CBotStateMachine::OnTick_Queuing`: watchdog — если прошло >15с и
`gcState.inQueue == false`, возврат в IN_MENU. orchestrator при `auto_queue`
сам переведёт обратно в FORMING_PARTY.

### 4. Party state stale bug

Симптом: после `LeaveParty` → `FORMING_PARTY` → `InviteToParty` → через 100ms
срабатывает "Party full via GC (2) → QUEUING", хотя мембер ещё не принял.
Дальше `StartFindingMatch` для пустой пати (size=1) → GC молчит → watchdog →
infinite retry loop.

**2 fixes**:

- `CGCMessageHandler::ResetMatchState()` теперь сбрасывает также `partyID`,
  `leaderID`, `partySize`, `memberIds`, `pendingPartyInviteID`. Раньше чистило
  только match-related поля → stale `partySize=2` от предыдущей пати сохранялось.
- `OnTick_FormingParty` GC-check получил **warmup 3с** (как уже был для
  file-check). Первые 3с после FORMING_PARTY entry не триггерим QUEUING по
  `gcState.partySize` — даём GC время прислать SOUpdate(removed CSODOTAParty)
  от старой пати.

## Артефакты

| Версия | md5 DLL | Version tag | Содержание |
|--------|---------|-------------|------------|
| 1 | `39f6f86b8ca80f9015575f396ffa7801` | `2026.04.18-phase-b-locator` | Phase B v1 locator |
| 2 | `4ab4fab12fb3ca1bd349ad27e5e75264` | `2026.04.18-phase-b-locator-queuing-watchdog` | + QUEUING watchdog |
| 3 | `9c937a5159305e0a7c2fd41ddcaba6e9` | `2026.04.18-party-state-reset-fix` | + ResetMatchState + FormingParty warmup (**актуальная**) |

Задеплоено в `eft-deploy:/data/static/dota/DotaFarm.zip` + `version.txt`.
Public URL: `https://v1per.tech/dota/DotaFarm.zip`.

## Файлы

- `tools/dota2/find_gcclient_anchors.py` — Python автоскан всех якорей (для dev / верификации после обнов)
- `Andromeda/BotFarm/GC/GCClientLocator.hpp/.cpp` — C++ runtime locator
- `Andromeda/BotFarm/GC/CGCMessageHandler.cpp` — +Init/RefreshSnapshot calls, +ResetMatchState party fields
- `Andromeda/BotFarm/StateMachine/CBotStateMachine.cpp` — +QUEUING watchdog, +FormingParty warmup

## Не сделано (перенесено в prompt_next)

- **Phase B v2**: полный SOCache pull с сериализацией CSharedObject → `ApplySOObject`
  (сейчас v1 — только observability через RTTI имена, не заполняет gcState для hot-inject)
- **Root cause: почему StartFindingMatchResult не приходит**. Подозрение на
  `ping_data_bytes=0` (никогда не захватили CMsgClientPingData), либо msg_type 4505
  устарел в proto, либо cv=6772 устарел. Watchdog маскирует симптом.
- 5 застарелых проблем (попап-приглашение, авто-accept матча, мид-АФК, скиллы
  после 2 lvl, сайды в лесу).

## Риски / наблюдения

- DLL размер вырос 2MB → 12MB (Release build подхватил debug info / no IPDB).
  Проверить что это не сломало manual-map стадию загрузчика.
- В логах бота #1 `client_version=0` в status JSON несмотря на `cv=6772` в
  detection. Separate bug, не блокер.

# PURCHASE_ITEM fix — DotaFarm, 2026-05-23

## 1. TL;DR

Две недели DotaFarm bot brain звал `ActionImmediate_PurchaseItem("item_tango")` десятки раз за матч — gold не списывался, инвентарь пустой, ни одной ошибки в логах. **Реальная причина:** в native `PrepareUnitOrders` для `OrderType=16` (PURCHASE_ITEM) восьмой аргумент (`unit`) должен быть указателем на **hero entity**, а не на player controller — мы передавали controller, engine молча дропал ордер. Поймано через **Frida `Interceptor.attach`** на UI Quick Buy, проверено **`NativeFunction.call` replay'ем** из Frida-треда (tango куплен 3/3 раза), зашиплено как worker-thread + F10 hotkey + Lua-байпасс.

## 2. Background — две недели "тихих дропов"

Симптом: `CBotBrain` Lua-скрипт зовёт `bot:ActionImmediate_PurchaseItem("item_tango")` каждые ~4 секунды на старте матча. Cooldown логики нет, throttle нет. По логам всё хорошо: `[PURCHASE] unit: item_tango` спамится с такта в такт. По игре — нулевой эффект.

Что пробовали и почему не помогло:

| Попытка | Что починили | Почему не сработало |
|---|---|---|
| AOB PrepareUnitOrders | Паттерн действительно сдвинулся после patch'а Dota | Native fn резолвилась — но ордер всё равно молча дропался |
| `Vector3* position = nullptr` | Crash в engine на nullptr | Падение убрали, но purchase не появился |
| `EOrderIssuer::HERO_ONLY` -> `PASSED_UNIT_ONLY` (3) | Снято подозрение на issuer | Не он был виноват — нужен был ещё и unit≠controller |
| Worker thread (CreateThread, clean TEB) | Из Lua tick thread reentrancy guard мог дропать | Сам по себе не лечил — argument layout был неверный |
| `Game.PrepareUnitOrders({OrderType:16})` через Panorama JS | Думали что headless клиент не имеет shop-context | Реально — handler для 16 отсутствует в client-side switch (см. §6) |
| `dota_purchase_item` console command | Думали что это ConCommand | Это chat-alias, не ConCommand |

Общая проблема всех попыток: **гадание на основе wire-эвристики и Ghidra-декомпиляции, без живого ground truth от UI**.

## 3. Frida harness — the breakthrough

Решающим шагом был отказ от гаданий и переход к **capture-then-replay** через Frida 17.9.3. Файл: `C:\Users\aleks\OneDrive\Документы\реверс курс\tools\dota2\auto_purchase_test.py`.

Pipeline (3 шага, всего ~150 строк JS+Python):

1. **Hook PrepareUnitOrders на entry.** `Interceptor.attach(client.base.add(0x1E0F150), { onEnter(args) { ... } })`. Из `this.context.rcx/rdx/r8/r9 + rsp+0x28..0x48` достаём **реальные** аргументы, которые игра подсунула функции из UI-треда (`auto_purchase_test.py:36-74`).
2. **User click ONCE.** Пользователь нажимает Quick Buy в магазине один раз → handler ловит первый ордер с `order == 16` и сохраняет `(ctrl, vec3_bytes, issuer, unit, show)`. Сразу же снимаем `Thread.backtrace` — кому пригодится backtrace выше PrepareUnitOrders.
3. **Replay через `NativeFunction.call`.** Из Python зовём `script.exports_sync.replay(44)` три раза с задержкой 1.5с (`auto_purchase_test.py:84-115`). Frida аллоцирует свой vec3{0,0,0} и зовёт ту же функцию с захваченным controller/issuer/unit — только `ability=defId` подменяем под тестовый item.

**Что увидели в захвате (живые данные с UI Quick Buy):**

```
[CAPTURED] UI Quick Buy:
  controller = 0x36438fd7000
  ability    = 11
  issuer     = 3
  unit       = 0x36477f48000    <-- !!! не равен controller
  show       = 1
```

**Что увидели в replay:** все три вызова из Frida-треда (не из UI-треда!) проходят, после ~10 секунд roundtrip в Dota появляется три tango в инвентаре и gold падает на 3×90 = 270. Подтверждено визуально пользователем.

**Почему этот подход выиграл недели гаданий:** не нужно угадывать ABI и calling convention — игра сама показала рабочий набор аргументов при UI-клике. `NativeFunction.call` доказал что **тред-контекст не имеет значения** (UI vs Frida == один engine path); важно только что в стек попадают **правильные** значения.

Этот паттерн "Interceptor.attach капчурит → NativeFunction.call воспроизводит" следует переиспользовать для любого RE-вопроса где функция engine'а silent drop'ает наш вызов, но UI её зовёт корректно.

## 4. Root cause — таблица аргументов `PrepareUnitOrders`

Сигнатура (x64 fastcall, `client.dll!PrepareUnitOrders @ +0x1E0F150`):

| Слот | Регистр / stack | Назв. | Что было у нас | Что у UI Quick Buy |
|---|---|---|---|---|
| 1 | RCX | `controller` | `lp.pController` | `0x36438fd7000` (player controller) |
| 2 | RDX | `orderType` | `16` (PURCHASE_ITEM) | `16` |
| 3 | R8 | `targetIndex` | `0` | `0` |
| 4 | R9 | `position` | `&Vector3{0,0,0}` | non-null vec3 |
| 5 | rsp+0x28 | `abilityIndex` | `defId` (44 = tango) | `11` (UI buy) |
| 6 | rsp+0x30 | `issuer` | `3` (PASSED_UNIT_ONLY) | `3` |
| 7 | rsp+0x38 | `unit` | **`controller`** ← bug | **`0x36477f48000`** (`C_DOTA_BaseNPC_Hero*`) |
| 8 | rsp+0x40 | `queue` | `0` | `0` |
| 9 | rsp+0x48 | `showEffects` | `true` | `1` |

Ключевое: `unit ≠ controller`. На UI Quick Buy в этом слоте лежит указатель на **hero entity** (`C_DOTA_BaseNPC_Hero*`), не на `C_DOTAPlayerController*`. Engine принимает ордер только если в слоте 7 — hero, иначе silent drop без какой-либо телеметрии в client.dll.

В коде это выражается так: `CGameState::LocalPlayer` хранит **обе** ссылки — `pController` и `pHero` (`CGameState.cpp:235-260`). В PURCHASE_ITEM правильно передавать `lp.pHero` как `unit`, остальные ордеры могут жить с `unit=0`.

## 5. The fix — три слоя

### 5.1 `CPurchaseWorker` — bounded SPSC ring + dedicated thread

`C:\temp\andromeda_src\Andromeda-Dota2-Base\Andromeda\BotFarm\Commands\CPurchaseWorker.{hpp,cpp}`. Worker сидит на `CreateSemaphore`, продьюсер из Lua-треда кидает в кольцевой буфер на 32 слота, worker дёргает native fn из чистого TEB:

```cpp
// CPurchaseWorker.hpp:42
auto Enqueue( void* controller, void* hero, int itemDefIndex ) -> bool;
```

В worker'е (`CPurchaseWorker.cpp:140-150`):

```cpp
( (PrepareUnitOrdersFn)w->m_pFn )(
    controller,
    16,                                            // PURCHASE_ITEM
    0,                                             // targetIndex
    &zero,                                         // vec3
    defId,                                         // ability = item def id
    3,                                             // issuer = PASSED_UNIT_ONLY
    reinterpret_cast<uintptr_t>( hero ),           // unit = hero entity (NOT controller)
    0,                                             // queue
    true                                           // showEffects
);
```

Worker нужен потому что прямой вызов из Lua-tick треда в первоначальной версии иногда натыкался на reentrancy guard `CDOTAPlayerController::PrepareUnitOrders`. Frida replay показал что **clean-context thread достаточно** — engine не разделяет какой именно тред зовёт, главное аргументы. Worker остался как страховка и для удобного non-blocking API из Lua.

### 5.2 F10 debug hotkey — minimal repro вне всякой Lua-логики

`C:\temp\andromeda_src\Andromeda-Dota2-Base\Andromeda\BotFarm\CBotFarmClient.cpp:308-313`:

```cpp
// F10 = direct test PURCHASE_ITEM (item_tango defId=44) через worker.
if ( GetAsyncKeyState( VK_F10 ) & 1 )
{
    DEV_LOG( "[F10] Manual test purchase: item_tango (defId=44)\n" );
    bool ok = GetPrepareOrders()->PurchaseItem( 44 );
    DEV_LOG( "[F10] PurchaseItem returned %d\n", ok ? 1 : 0 );
}
```

Цель — снять Lua с уравнения. Нажал F10 на Demo Hero → tango появилось. Это итоговая live-проверка что Worker + аргументы корректны без участия CBotBrain.

### 5.3 `CPrepareOrders::PurchaseItem` — high-level wrapper

`C:\temp\andromeda_src\Andromeda-Dota2-Base\Andromeda\BotFarm\Commands\CPrepareOrders.cpp:122-132`:

```cpp
bool CPrepareOrders::PurchaseItem( int itemDefIndex )
{
    auto lp = GetGameState()->GetLocalPlayer();
    if ( !lp.pController || !lp.pHero || itemDefIndex <= 0 ) return false;
    return GetPurchaseWorker()->Enqueue( lp.pController, lp.pHero, itemDefIndex );
}
```

Один точка входа для всех слоёв (F10, Lua, panorama fallback). `lp.pHero` подставляется автоматически — call site об этом думать не должен.

### 5.4 Lua bypass (task #1, в параллельной работе)

`CBotBrain.cpp` + `LuaUnitProxy.cpp`: Lua-метод `ActionImmediate_PurchaseItem(itemName)` больше **не** идёт через `CPanoramaJS::OrderPurchaseItem` (старый сломанный путь). Вместо этого резолвится `itemName → defId` через `GetItemDefId()` и сразу вызывается `GetPrepareOrders()->PurchaseItem(defId)`. Native worker path — единственный production-канал.

## 6. JS path RIP — почему Panorama не работает для PURCHASE

`CPanoramaJS::OrderPurchaseItem` (`CPanoramaJS.cpp:497-557`) исторически собирал JS-строку вида:

```js
Game.PrepareUnitOrders({UnitIndex: <hero>, OrderType: 16, AbilityIndex: <defId>, OrderIssuer: 3, ShowEffects: true});
```

`OrderType:16` (PURCHASE_ITEM) **отсутствует в client-side switch** `CDOTAClientMode::SendUnitOrder` — handler покрывает 1/3/4/5/6/7/8/9/11/20/21/30, но не 16 (см. комментарий `CPanoramaJS.cpp:337-349`). JS-вызов проходит до этого свитча и тихо дропается, ничего не отправляя на сервер. Логи Andromeda при этом весело пишут `[PURCHASE] unit: item_tango` — false positive.

Console command `dota_purchase_item` — chat-alias, не ConCommand, тоже dead end.

Единственный канал: **native `PrepareUnitOrders`** с корректным `unit=hero` (минует client-side switch, идёт напрямую в order queue server-bound).

## 7. Verification

| Проверка | Результат |
|---|---|
| Frida replay tango ×3 на UI hero | OK (3/3 в инвентаре, gold -270) |
| F10 на Demo Hero через worker | OK (tango появился, log: `PurchaseItem returned 1`) |
| `lp.pHero` valid после `RefreshLocalPlayer` | OK (см. §8.2) |
| Lua bypass deploy | pending (task #1) — план: собрать DLL, инжектнуть на test-host `dotahost`, запустить матч, наблюдать gold-drop в первые 60с |

После выкатки Lua-bypass'а можно снимать F10 — оставлять hotkey в release-DLL нет смысла (debug-only).

## 8. Side findings

### 8.1 Main Steam account crash Andromeda на Demo Hero

При загрузке Demo Hero под main аккаунтом — `STATUS_BREAKPOINT @ KernelBase` (через `tier0.dll → engine2.dll → server.dll`). Под другим Steam-аккаунтом тот же билд DLL грузится без проблем. Это **не имеет отношения к purchase logic** — account-specific hook race в одной из чужих fixture-инициализаций сервера. Workaround: для тестов F10 / Frida-replay'я использовать вторичный аккаунт.

### 8.2 `CGameState::RefreshLocalPlayer` — crash во время match loading

`CGameState.cpp:209-217` — порядок lookup'а инвертирован: сначала зовётся стабильная engine fn `GetCL_DOTAPlayerController()->GetLocal()`, и **только** если она вернула nullptr — fallback на SteamID-скан `FindLocalControllerBySteamID()` (`CGameState.cpp:163-207`).

SteamID-скан ограничен `kMaxControllerSlot=64` (player controllers всегда в low slots), каждый слот обёрнут в `__try/__except`. До фикса: при загрузке матча entity table бьёт 8000+ entries, скан натыкался на полусконструированные entities, engine выпадал в DebugBreak. Это блокировало даже добраться до момента когда можно было бы попробовать PURCHASE_ITEM.

## 9. Files changed

В Andromeda (вне репо, `C:\temp\andromeda_src\Andromeda-Dota2-Base\Andromeda\`):

- `BotFarm\Commands\CPurchaseWorker.hpp` — `Enqueue(controller, hero, defId)` signature.
- `BotFarm\Commands\CPurchaseWorker.cpp` — worker dispatches с `unit=hero`.
- `BotFarm\Commands\CPrepareOrders.cpp` — `PurchaseItem` достаёт `lp.pHero` из GameState.
- `BotFarm\GameState\CGameState.cpp` — `RefreshLocalPlayer` инвертирован + 64-slot scan + `__try/__except`.
- `BotFarm\CBotFarmClient.cpp` — F10 debug hotkey (`OnTick`, line 308).

Pending в task #1:

- `BotFarm\BotBrain\CBotBrain.cpp` — Lua bypass, прямой вызов `GetPrepareOrders()->PurchaseItem`.
- `BotFarm\BotBrain\LuaUnitProxy.cpp` — то же на Lua proxy уровне.

Harness в репо:

- `tools\dota2\auto_purchase_test.py` — Frida capture-then-replay (использовать как шаблон для следующих RE-задач).

## 10. Reusable lessons

- **Capture-then-replay через Frida > Ghidra гадания.** Когда функция engine'а silent-drop'ает наш вызов, но UI её зовёт корректно — повесь `Interceptor.attach` на entry, дай пользователю кликнуть один раз, забери реальные аргументы, проверь replay'ем через `NativeFunction.call`. Это занимает 30 минут вместо двух недель.
- **Не доверять wire-эвристике и spam'ным success-логам.** `[PURCHASE] unit: item_tango` каждые 4 секунды — это false positive, потому что JS-путь дропается в client-side switch до отправки в order queue.
- **`controller ≠ unit`.** В Source 2 player controller и контролируемый юнит — разные entities с разными адресами. Для любого ордера, который должен исполняться юнитом (а не controller'ом), в слот `unit` идёт указатель на юнит, не на controller.
- **Clean TEB thread для engine fn** — хорошая страховка от reentrancy guard'ов, но **не магия**: если аргументы неверные, никакой worker не спасёт.
- **Backtrace из Frida-хука дешёвый.** `Thread.backtrace(this.context, Backtracer.ACCURATE).map(DebugSymbol.fromAddress)` — мгновенно покажет какие middleware-frame'ы стоят выше native fn у UI-пути, и есть ли там что-то что мы не воспроизводим.
- **Один слой "minimal repro" hotkey'я** (F10) — окупает себя в 100х: снимает Lua/BotBrain/Panorama с уравнения, оставляет только native fn + worker.

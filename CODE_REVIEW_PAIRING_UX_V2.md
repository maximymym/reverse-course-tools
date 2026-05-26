# Code Review: Pairing UX v2

## Summary

- Files reviewed: `pair_code.{h,cpp}`, `pair_code_test.cpp`, `orchestrator_relay_client.{h,cpp}`, `match_pairing_fsm.h`, `sync_start_coordinator.{h,cpp}`, `config.{h,cpp}` (pairing sections), `theme.{h,cpp}` (CopyToClipboard, Pill), `orchestrator.{h,cpp}` (ReinitPairing, GuardedStartFarm, wiring), `gui.cpp` (Pairing panel, 3 modals, START FARM gating), `CMakeLists.txt`
- Lines diff: ~1 800 net new
- **Verdict: APPROVE WITH CHANGES** — 0 Critical, 2 High, 4 Medium, 5 Low

---

## Critical findings (block merge)

_None._

---

## High priority (should fix before deploy)

### H1 — `RelayPeer::SetState` callback fires while `m_onState` может быть nullptr после Stop

**File:** `orchestrator_relay_client.cpp:30-42`

```cpp
void RelayPeer::SetState( State s )
{
    State prev = m_state.exchange( s );
    if ( prev == s ) return;
    std::function<void( State )> cb;
    {
        std::lock_guard<std::mutex> lk( m_errMx );
        cb = m_onState;   // копируем под lock
    }
    if ( cb ) cb( s );    // вызываем вне lock — OK
}
```

`Stop()` зануляет `m_onState` под `m_errMx` (`orchestrator_relay_client.cpp:128-130`), но между `m_state.exchange` и захватом `m_errMx` в `SetState` есть окно: `RunLoop` ещё выполняется (последний `SetState(Disconnected)` на `orchestrator_relay_client.cpp:399`), `Stop()` уже вышел, `m_thread.join()` завершился, `m_onState` обнулён. В этом сценарии `cb` будет скопировано как `nullptr` — и вызова не будет. **Фактически это безопасно**, но поведение строго зависит от порядка операций: `m_thread.join()` в `Stop()` выполняется **до** обнуления `m_onState` (`orchestrator_relay_client.cpp:125-129`), что означает, что после `join()` `RunLoop` уже завершил финальный `SetState`. Поэтому race-окна нет.

Однако существует другой сценарий: `RequestReconnect()` закрывает сокет под `m_sendMx`, после чего `RunLoop` переходит к `disconnect:` и вызывает `SetState(Disconnected)` — а `onRelayState` callback в `orchestrator.cpp` (приведённый через лямбду `[this]`) к этому моменту может быть вызван уже **после** того как `Orchestrator` начал деструктуроваться, если `RequestReconnect` вызывается из GUI в позднем шатдауне.

**Fix:** В `Orchestrator::~Orchestrator` (или в `StopFarm`) явно вызывать `m_relayPeer->Stop()` перед тем как разрушатся члены orchestrator'а, на которые ссылается lambda. Добавить в деструктор `Orchestrator`:
```cpp
if ( m_relayPeer ) { m_relayPeer->Stop(); m_relayPeer.reset(); }
```
Если деструктор уже это делает через `ReinitPairing` — убедиться что он вызывается.

---

### H2 — `SyncStartModal`: popup не закрывается через `ImGui::CloseCurrentPopup()` при state-переходе вне кнопки

**File:** `gui.cpp:1909-1916`

```cpp
if ( !show )
{
    if ( wasOpened )
    {
        wasOpened = false;  // coordinator advanced state → drop popup
    }
    return;   // <--- возврат без CloseCurrentPopup
}
```

Когда `SyncStartCoordinator` уходит из `PEER_REQUESTED` (например по timeout auto-decline или по Reset из peer disconnect), `show` становится `false`. Функция сбрасывает `wasOpened` и возвращается — но **не вызывает `ImGui::CloseCurrentPopup()`**. Результат: ImGui держит popup открытым до следующего `EndPopupModal` который никогда не придёт, что вызывает assertion/hang в `EndFrame` в debug-сборке и visual freeze в release.

В отличие от `RenderPairCodeGenerateModal_` и `RenderPairCodePasteModal_`, которые просто возвращаются когда флаг сброшен (там popup закрывается кнопкой), в `RenderSyncStartModal_` popup может быть закрыт **снаружи** (FSM timeout).

**Fix:**
```cpp
if ( !show )
{
    if ( wasOpened )
    {
        wasOpened = false;
        ImGui::CloseCurrentPopup();  // добавить
    }
    return;
}
```
Либо добавить вызов `ImGui::CloseCurrentPopup()` внутри `BeginPopupModal`-блока перед `return` когда `!show`.

---

## Medium priority (post-deploy cleanup OK)

### M1 — `Error::SameMachine` объявлен в enum, но никогда не используется в `Decode()`

**File:** `pair_code.h:20`, `pair_code.cpp` — весь файл

`Error::SameMachine` присутствует в enum и описан в `Describe()` (`pair_code.cpp:173`), но в `Decode()` нет ни одной проверки которая его эмитирует. `Error::SameMachine` задуман для предотвращения self-pairing (когда мастер случайно вставляет свой же код). Без этой проверки оба orchestrator'а могут подключиться к одному pair_id с одной ролью — relay или FSM это не поймает явно, просто ничего не произойдёт.

**Fix:** Либо реализовать проверку (захешировать `userId+pairId` при генерации, хранить в сессии, сравнивать при decode), либо убрать `SameMachine` из enum пока не реализовано — мёртвый код в error path вводит в заблуждение.

---

### M2 — `configHash = "todo-hash"` в `GuardedStartFarm`

**File:** `orchestrator.cpp:3076`

```cpp
std::string configHash = "todo-hash";
if ( !m_syncStart.Initiate( m_config.pairing.role, configHash ) )
```

`configHash` едёт в `start_request` body и на peer side используется для sanity check. Сейчас всегда `"todo-hash"` — peer не может ничего проверить. Это не security issue (секрет — HMAC в протоколе), но при будущей реализации проверки на peer side оба оркестратора должны получить одинаковый хэш (одинаковый config), иначе будут ложные mismatches.

**Fix:** Вычислить реальный хэш (CRC32 или SHA-1 от ключевых полей `FarmConfig` — heroes, region, gameMode, role). Или явно задокументировать что поле игнорируется на обоих концах.

---

### M3 — `RequestDisconnect()` не проверяет `State::RUNNING` — можно дисконнектиться во время активного матча

**File:** `orchestrator.cpp:3103-3119`

```cpp
void Orchestrator::RequestDisconnect()
{
    if ( m_relayPeer ) { m_relayPeer->Stop(); ... }
    m_pairing.Reset();
    m_syncStart.Reset( "user_disconnect" );
}
```

Нет guard'а: `RequestDisconnect()` доступна из GUI в любой момент, включая `State::RUNNING` с активным матчем. Это оборвёт relay-соединение в середине farm-цикла. Peer не получит graceful disconnect (только TCP close). `MatchPairingFsm` сбросится — в том числе потеряет уже накопленные lobby_ids.

**Fix:** Добавить предупреждение в GUI ("Ферма запущена — отключение прервёт матч") или запретить Disconnect кнопкой когда `State::RUNNING`. Аналогично `ReinitPairing` — там уже есть guard.

---

### M4 — `SaveFarmBoolSetting` / `SaveFarmStringSetting` / `SaveFarmIntSetting` не атомарны и не используют LockFileEx

**File:** `config.cpp:490-518`

```cpp
bool SaveFarmBoolSetting( ... )
{
    json j = json::object();
    { std::ifstream in( path ); ... in >> j; }  // read
    j[key] = value;
    std::ofstream out( path );                  // write (НЕ атомарно!)
    out << j.dump( 4 );
}
```

В отличие от `SavePairingConfigAtomic` (который использует `FileLock` + `MoveFileExA`), эти три хелпера пишут напрямую через `std::ofstream`. Если два вызова придут одновременно (GUI toggle + background save) — файл будет повреждён или один writes перезатрёт другой.

**Fix:** Использовать тот же `FileLock` + `WriteAtomicFile` паттерн что и в `SavePairingConfigAtomic`. Или выделить единый `SaveFarmJson` helper с lock'ом и переиспользовать во всех четырёх функциях.

---

## Low / nits

### L1 — `Pill()` в `theme.cpp:711` не вызывает `ImGui::Dummy` для layout reservation

**File:** `theme.cpp:703-729`

Функция рисует через `dl->AddRectFilled` и `dl->AddText`, но не резервирует layout space через `ImGui::Dummy(size)`. По контракту из комментария в `theme.h:200` ("reserves layout space via ImGui::Dummy so it composes with SameLine()") — Dummy должен быть вызван. Без него следующий `SameLine()` или `Text()` перекроет Pill.

Проверить что все call-сайты `Pill()` явно добавляют `ImGui::SameLine()` и `ImGui::Dummy` — иначе вёрстка едет.

---

### L2 — `%013lld` в `GenerateRequestId_()` — MSVC-специфичный format specifier

**File:** `sync_start_coordinator.cpp:28`

```cpp
std::snprintf( buf, sizeof( buf ), "%013lld-%08x", (long long)ms, rnd );
```

На MSVC `%lld` поддерживается с VS2015+, здесь это нормально. Но `I64d` — не нужен. Замечание: явный cast `(long long)ms` уже нейтрализует warning. Оставить как есть, но добавить `#pragma warning(disable: 4996)` если CI даёт C4996 на snprintf.

---

### L3 — `config.cpp:103-112` — debug log в `C:\temp\andromeda\config_load.log` в production коде

**File:** `config.cpp:103-112`

```cpp
FILE* f = fopen( "C:\\temp\\andromeda\\config_load.log", "a" );
if ( f ) { fprintf(...); fclose( f ); }
```

Hardcoded путь в `C:\temp\andromeda\`. Пишет на каждый `LoadAccounts` вызов (каждый перезапуск фермы). На стенде нормально, но в distrib-версии засоряет диск юзера. Обернуть в `#ifdef _DEBUG` или использовать существующий `Log()` orchestrator'а.

---

### L4 — `pair_code_test.cpp:159-206` — `BuildCode` дублирует логику `Base64UrlEncode` из `pair_code.cpp`

Тест содержит свою копию base64url encoder. При изменении алфавита или padding-политики в `pair_code.cpp` тест может остаться несинхронизированным и давать ложные PASS. Рефакторинг: сделать `pair_code::internal::Base64UrlEncode` доступным в `#ifdef PAIR_CODE_TEST` или перейти к black-box тестам через `Encode()` + JSON-инъекцию через другой механизм.

---

### L5 — `RenderSyncStartModal_`: таймер deadline использует `GetTickCount64()` вместо steady_clock

**File:** `gui.cpp:1932-1938`

```cpp
const int64_t now      = (int64_t)GetTickCount64();
const int64_t deadline = ps.syncStart.ackDeadlineMs;  // из SyncStartCoordinator — steady_clock ms
const int secsLeft = ( deadline > 0 ) ? (int)( ( deadline - now ) / 1000 ) : -1;
```

`SyncStartCoordinator::NowMs_()` использует `steady_clock` (`sync_start_coordinator.cpp:9-12`), а GUI читает `GetTickCount64()`. На Windows `GetTickCount64()` и `steady_clock::now()` отсчитывают от одного эпоха (boot time), и обычно совпадают, но это **implementation-defined** — на некоторых конфигурациях `steady_clock` может использовать QPC с другим эпохом. Результат: `secsLeft` может быть существенно неверным (~0 или ~огромное число), модал покажет мусор.

**Fix:** Использовать тот же источник — `std::chrono::steady_clock::now()`:
```cpp
using namespace std::chrono;
int64_t now = duration_cast<milliseconds>( steady_clock::now().time_since_epoch() ).count();
```

---

## Positive notes (well-done patterns)

1. **PendingActions pattern в `SyncStartCoordinator`** — все callbacks (broadcast, startFarmFn, onStateChange) выполняются строго после release mutex'а. Паттерн правильно предотвращает deadlock через re-entry (broadcast → relay send → peer → OnPeerMessage). Чистое и безопасное решение.

2. **`RelayPeer` atomic telemetry** — `m_msgSent`, `m_msgRecv`, `m_lastRttMs`, `m_lastActivityMs`, `m_state`, `m_connected` — все `std::atomic`. `GetSnapshot()` корректно использует отдельный lock только для строковых полей (`m_lastError`, `m_lastRelayErrorCode`). Lock-contention на GUI polling (~10Hz) минимален.

3. **`SavePairingConfigAtomic`** — правильный RMW паттерн: FileLock (LockFileEx на `.lock`-файл) + MoveFileExA для атомарной замены. Защита от corrupt-файла (catch→return false вместо перезаписи). Preserve non-pairing секций.

4. **`pair_code::Decode`** — defensive parsing: все 10 полей проверены на тип + диапазон до записи в `Decoded`. `SameMachine` не реализован, но все остальные 9 error path покрыты тестами. CRC8 перед base64 decode — правильный порядок (fail fast на corruption).

5. **`Decode` секция для pairSecret** — толерантность к обоим форматам (standard base64 с padding + base64url без) реализована аккуратно через strip-and-translate. Это нужно потому что разные генераторы (OpenSSL vs Go's base64.RawURLEncoding) дают разные форматы.

6. **`GuardedStartFarm` guards** — три проверки перед handshake: connected, hb < 5s, syncStart == IDLE. Ни одна из них не может быть обойдена из GUI — правильное layering.

7. **Тест `pair_code_test.cpp`** — покрывает все 9 реализованных error enum values + round-trip + whitespace tolerance + оба secret форматы. Standalone exe без gtest dependency — хорошо для CI без зависимостей.

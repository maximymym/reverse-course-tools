# Silent Failure Audit: Pairing UX v2

## Summary
- Files audited: `pair_code.cpp`, `pair_code.h`, `sync_start_coordinator.cpp`, `sync_start_coordinator.h`, `match_pairing_fsm.cpp`, `match_pairing_fsm.h`, `orchestrator_relay_client.cpp`, `orchestrator_relay_client.h`, `config.cpp`, `config.h`, `orchestrator.cpp`, `orchestrator.h`, `gui.cpp`
- Verdict: **PASS WITH NOTES**

---

## Critical (silent failures that hide real problems from user)

_None found._

---

## High (potential silent failures in edge cases)

### H1 — GuardedStartFarm: TOCTOU race between peer-connected check and Initiate
**File:** `orchestrator.cpp:3052-3081`  
**Scenario:** `GetPairingStatus()` is called under no lock. It reads `m_relayPeer->IsConnected()` (atomic) and `LastPeerActivityMs()` (atomic) at one moment. Between the `ps.connected` / `ps.lastPeerHbAgeMs < 5000` checks passing and `m_syncStart.Initiate()` being called, the relay socket can close (peer drops). `Initiate` succeeds and transitions `SyncStartCoordinator` to `WAITING_PEER_ACK`. The broadcast `start_request` message is sent via `SendPairingMessage_` → `m_relayPeer->Send()`, which is a silent no-op when socket is `INVALID_SOCKET` (relay_client.cpp:408). The peer never receives the request. The initiator then sits in `WAITING_PEER_ACK` for 15 s until `ackTimeoutMs` fires and transitions to `TIMEOUT`. The user sees "TIMEOUT" in the panel — **not** silent, but the root cause (dropped socket between check and send) is invisible.  
**Observable symptom:** "TIMEOUT" badge in GUI, orchestrator log shows `[sync-start] state → 6 (TIMEOUT)`. No log entry explaining that the broadcast failed because the socket was already dead.  
**Suggested fix:** After `Initiate()` succeeds, call `SendPairingMessage_` is called inside `Flush_` from within `SyncStartCoordinator`. That call reaches `RelayPeer::Send` which already silently no-ops. Add a log in `Send()` when `m_sock == INVALID_SOCKET` to surface the drop, e.g.:
```cpp
if ( m_sock == INVALID_SOCKET ) {
    // log once per disconnect cycle
    return;
}
```
Or check `IsConnected()` after `Initiate()` and immediately cancel with a user-visible error instead of waiting 15 s for ack timeout.

---

### H2 — ApplyPairCodeAndReinit: in-memory state mutated even when SavePairingConfigAtomic fails
**File:** `orchestrator.cpp:3121-3149`  
**Scenario:** `config::ApplyPairCode()` patches `m_config.pairing` in-memory (relayHost, userId, pairSecret, transport, role, enabled=true, uxV2=true) **before** `SavePairingConfigAtomic` is called. If `SavePairingConfigAtomic` returns `false` (disk full, lock contention, corrupted farm.json), the function returns `false` and logs an error. However, `m_config.pairing` is already permanently mutated. On the next `InitPairing_()` call (e.g. next app restart without the file being fixed), the new credentials will be used from memory even though they were never persisted. Conversely, the next `LoadFarmSettings` on restart will reload the old file, creating a session-only divergence that's confusing to diagnose.  
**Observable symptom:** GUI shows "APPLY FAILED — see orchestrator log", but relay connection attempts immediately start using the new credentials in-memory (until next restart). The log message correctly says `SavePairingConfigAtomic FAILED` but it's easy to miss if the user dismisses the dialog.  
**Suggested fix:** Save a copy of the old PairingConfig before `ApplyPairCode`, and restore it on failure:
```cpp
auto savedPairing = m_config.pairing;   // snapshot
config::ApplyPairCode(decoded, m_config.pairing);
m_config.pairing.uxV2    = true;
m_config.pairing.enabled = true;
if (!config::SavePairingConfigAtomic(path, m_config.pairing)) {
    m_config.pairing = savedPairing;    // rollback
    Log(...);
    return false;
}
```

---

### H3 — WriteAtomicFile: no FlushFileBuffers before MoveFileEx, .tmp not cleaned on MoveFileEx failure
**File:** `config.cpp:565-583`  
**Scenario A (fsync gap):** The `std::ofstream` is flushed (`f.flush()`) but the OS write-back cache is not forced to disk before `MoveFileEx`. On Windows, `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING` is itself not guaranteed to be durable on a power failure mid-write. The tmp file's data may still be in the page cache. For a crash (power loss) between `MoveFileEx` completing in the filesystem journal and the actual sector write, the destination will point to the new file but its contents may be zero or partial on recovery. This is edge-case on NTFS with journaling, but a known risk on VMs (QEMU virtio-blk with `cache=writeback`).  
**Scenario B (MoveFileEx fail → stale .tmp):** When `MoveFileEx` fails (disk full, sharing violation), `DeleteFileA(tmpPath)` is called to clean up. However, `DeleteFileA` return value is ignored. On a locked file (unlikely but possible with AV scanning .tmp), the .tmp file is left behind. Subsequent calls to `SavePairingConfigAtomic` will overwrite it, so this is low-severity pollution.  
**Observable symptom (A):** Silent data loss after power failure — next boot reads old farm.json. No log entry.  
**Observable symptom (B):** Stale `farm.json.tmp` in config directory after unexpected failure.  
**Suggested fix (A):** Add `FlushFileBuffers(fileHandle)` before closing the ofstream, or open the file with `FILE_FLAG_WRITE_THROUGH`. Simplest in the current ofstream pattern is to close the stream, then open the handle for `FlushFileBuffers`:
```cpp
// After f.flush(); f.close() (scope exit):
HANDLE hFlush = CreateFileA(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
if (hFlush != INVALID_HANDLE_VALUE) { FlushFileBuffers(hFlush); CloseHandle(hFlush); }
```
**Suggested fix (B):** Log `DeleteFileA` failures.

---

### H4 — AuthFailed sticky state: no path for UI to prompt re-paste after auth_failed + backoff
**File:** `orchestrator_relay_client.cpp:315-323`, `orchestrator.h:229-245`  
**Scenario:** Relay returns `auth_failed`. `RelayPeer` sets `State::AuthFailed` (sticky), applies 60 s backoff, and keeps retrying indefinitely. The GUI shows `AUTH FAILED` badge (correct). However, after the user pastes a new pair code with corrected credentials via `ApplyPairCodeAndReinit` → `ReinitPairing`, the old `RelayPeer` is `Stop()`-ped and a new one is `Start()`-ed. This clears `AuthFailed`. So the correct-credentials path works.  
**The gap:** If `ReinitPairing` is refused because `m_state == RUNNING` (farm is running while user tries to fix credentials), the auth failure badge persists and the user has no path to reconnect without stopping the farm. The Reconnect button calls `RequestForceReconnect` which calls `m_relayPeer->RequestReconnect()` — this resets `m_authFailureBackoffMs` and closes the socket, causing an immediate retry. But the credentials in the running peer are **still the old (bad) ones** since `ApplyPairCode` was blocked by the `RUNNING` guard. The user sees relay cycling auth-fail → connect → auth-fail at 60 s intervals with no explanation that they must stop the farm first.  
**Observable symptom:** GUI shows AUTH FAILED, Reconnect button triggers faster retry but the error persists. Log shows `relay error code=auth_failed` repeatedly. No log or tooltip telling user "stop farm to update credentials".  
**Suggested fix:** In `ReinitPairing`, when refusing due to `RUNNING`, log a user-visible message that includes the specific guidance: `"[pairing] ReinitPairing refused while RUNNING — stop farm first to update pair credentials"`. Also consider showing this in the GUI panel when `relayErrorCode == "auth_failed" && state == RUNNING`.

---

## Medium (logging/observability improvements)

### M1 — SyncStartCoordinator: startFarmFn throw → STARTING state stuck forever
**File:** `sync_start_coordinator.cpp:88-95`  
**Scenario:** `startFarmFn()` is called inside `Flush_()` outside the mutex. If `startFarmFn` throws an exception (e.g. `std::thread` constructor fails under resource exhaustion), the exception propagates out of `Flush_()`. The state machine is already in `STARTING` and `m_ackDeadlineMs = nowMs + startingHoldMs`. The next `Tick()` call will fire the STARTING→IDLE transition normally after `startingHoldMs` (1 s default), so the FSM **self-recovers** — not permanently stuck. But the farm never actually starts, and there is no log at the call site about the exception.  
**Observable symptom:** GUI briefly shows "STARTING…" then returns to IDLE after ~1 s. No error message. User sees sync-start succeeded but farm doesn't launch.  
**Suggested fix:** Wrap `startFarmFn()` in try/catch in `Flush_`:
```cpp
if (actions.fireStartFarm && startFarmFn) {
    try { startFarmFn(); }
    catch (const std::exception& e) {
        // surface to caller via onStateChange or a separate error callback
    }
}
```

### M2 — CONFIRMING fallback in Tick: no log when it fires
**File:** `sync_start_coordinator.cpp:339-343`  
**Scenario:** `CONFIRMING` state has a 500 ms deadline. If `UserAccept()` somehow fails to do the CONFIRMING→STARTING transition synchronously (e.g. second lock acquisition fails because another thread already changed state), `Tick` fires the fallback `TransitionTo_(STARTING)`. This is a recovery path with `// Should have transitioned synchronously in UserAccept` comment but no `Log()` call.  
**Suggested fix:** Add a log when this fallback fires, e.g. `Log("[sync-start] WARN: CONFIRMING timeout fallback → STARTING")`.

### M3 — MatchPairingFsm: EmitDecision calls onBroadcast under m_mx
**File:** `match_pairing_fsm.cpp:85-111`  
**Scenario:** `EmitDecision` is called while `m_mx` is held (from `OnMatchPendingFile`, `OnPeerMessage`, `Tick` — all hold `m_mx`). `onBroadcast` calls `SendPairingMessage_` which calls `m_relayPeer->Send()`, which acquires `m_sendMx`. This is a second lock. If `RelayPeer::RunLoop` ever calls back into `OnPairingMessage` while holding `m_sendMx` (it does not currently, but could if refactored), this creates a lock-order inversion risk. Currently safe, but fragile.  
**Suggested fix:** Mirror the `SyncStartCoordinator` `PendingActions` pattern — collect broadcast data inside the lock, emit outside. Low priority given current call paths.

### M4 — GetPairingStatus: hb age computed from NowMs() called twice
**File:** `orchestrator.cpp:2808-2826`  
**Scenario:** `crash_watchdog::NowMs()` is called up to 3 times in `GetPairingStatus()` (relay path, master path, slave path). Between calls `NowMs()` is always monotonically increasing, so `lastPeerHbAgeMs` can be computed with a tiny off-by-a-few-ms. Not a bug, just unnecessary drift. Low impact.

### M5 — SaveFarmBoolSetting / SaveFarmStringSetting / SaveFarmIntSetting: non-atomic plain ofstream
**File:** `config.cpp:448-518`  
**Scenario:** These three functions write farm.json via a plain `std::ofstream` without LockFileEx or MoveFileEx. A crash or power failure mid-write will leave a partially-written farm.json. Unlike `SavePairingConfigAtomic` these are used for toggle writes (e.g. `use_tun2socks` checkbox). If farm.json is corrupted, `LoadFarmSettings` silently falls back on next start (catches `...`), but the user's settings are lost.  
**Observable symptom:** After crash mid-toggle, farm.json may be truncated/empty. Next start loads defaults. No error shown to user.  
**Suggested fix:** Route these through `WriteAtomicFile` + `FileLock` as `SavePairingConfigAtomic` does. At minimum, use MoveFileEx pattern.

### M6 — RenderPairCodeGenerateModal: empty code shown without error
**File:** `gui.cpp:1716`  
**Scenario:** `s_code = orch.GenerateCurrentPairCode()` is called when the modal opens. `GenerateCurrentPairCode` returns `""` if `relayHost`, `userId`, `userAuthToken`, `pairId`, or `pairSecret` is empty. In that case `s_code` is empty. The modal renders `ImGui::InputText` (read-only) showing an empty string, and the COPY button will copy an empty string to clipboard. There is no error message telling the user why the code is empty.  
**Observable symptom:** User clicks GENERATE, sees blank input box and copies nothing. Invisible configuration problem.  
**Suggested fix:** Check `s_code.empty()` after generation and show an explicit error:
```cpp
if (s_code.empty())
    ImGui::TextColored(theme::V(theme::kColCrash),
        "ERROR: relay not configured. Paste a code first, or fill relay fields manually.");
```

---

## Low / Suggestions

### L1 — pair_code::Decode: SameMachine error variant is defined but never set
**File:** `pair_code.h:20`, `pair_code.cpp:160-176`  
`Error::SameMachine` is in the enum and has a `Describe()` string, but `Decode()` never emits it. If same-machine detection is planned, this is a future stub. Currently harmless — `Describe()` covers it.

### L2 — SyncStartCoordinator: GenerateRequestId_ uses system_clock for lex-comparable ID, but Tick uses steady_clock  
**File:** `sync_start_coordinator.cpp:22`, `sync_start_coordinator.h:146`  
`GenerateRequestId_` uses `system_clock` (wall time) for the timestamp prefix. `NowMs_()` uses `steady_clock`. `TransitionTo_` is called with `NowMs_()` (steady). If the machine clock is adjusted between calls, `ackDeadlineMs` drift is bounded (steady_clock is used for timeout arithmetic), but request IDs from different machines could have unexpected lex ordering if clocks are skewed by more than the tie-break period. Low risk in practice (the random suffix already breaks ties).

### L3 — RelayPeer: `m_lastRelayErrorCode` not cleared on successful re-connect
**File:** `orchestrator_relay_client.cpp:240-244`  
After a successful connect, `m_lastError` is set to `"connected"` and `m_lastRelayErrorCode.clear()` is called. This is correct. However, `GetSnapshot().lastRelayErrorCode` is read under `m_errMx` and the field is cleared atomically with the connect. Confirmed no issue — noting for completeness.

### L4 — MatchPairingFsm::Reset does not reset m_cancelStreak
**File:** `match_pairing_fsm.cpp:23-33`  
`Reset()` clears phase, localPending, peerLobbyIds, waitDeadlineMs, localMajority, pendingDecision, decisionConsumed — but **not** `m_cancelStreak`. `ResetCancelStreak()` exists as a separate call. `Init()` also does NOT reset `m_cancelStreak`. If `ReinitPairing()` is called (which calls `m_pairing.Init()`), the cancel streak survives. This is intentional if a re-init mid-session should preserve the streak, but could lead to unexpected `maxConsecutiveCancels` triggers after a config re-apply. Add a comment to clarify intent.

### L5 — onSyncStartMsg callback: called outside m_mx in MatchPairingFsm::OnPeerMessage, but before checking lock contention
**File:** `match_pairing_fsm.cpp:165-169`  
`onSyncStartMsg` is called before acquiring `m_mx`. The comment notes "Callback не под mutex — SyncStartCoordinator имеет свой." This is correct and intentional. No issue, but worth noting that the callback is invoked from whichever thread drives `OnPeerMessage` (relay recv loop). `SyncStartCoordinator::OnPeerMessage` acquires `m_mx` (its own mutex), so no deadlock.

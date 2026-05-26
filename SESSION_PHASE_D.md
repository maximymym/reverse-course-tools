# SESSION_PHASE_D — farm.json pairing config на стендах

**Date:** 2026-05-26
**Branch:** `feature/two-stand-coordination`
**Scope:** Phase D плана — включить pairing на обоих стендах с relay креденшелами.

## Состояние

| Стенд | IP | Role | farm.json patch | Lua deploy | Status |
|---|---|---|---|---|---|
| A (dotahost) | 217.60.245.131 | **master** / stand_a | ✅ done 2026-05-26 13:04:46 | ✅ done | готов к запуску |
| B (dotahost2) | 217.60.245.152 | **slave** / stand_b | ⏳ ждёт SSH bootstrap | ⏳ | блокирован Phase A.1 |

## Что сделано (.131)

### farm.json pairing block

Создан backup → `farm.json.bak-2026-05-26-130446-pairing`. Поля изменены через `tools/vds_deploy/patch_farm_pairing.ps1`:

```
enabled         : True              (было False)
transport       : relay             (было direct)
role            : master
user_id         : stand_a
user_auth_token : 1a9d0b80210bc74b8957c6ff13708051
pair_id         : two-stand-prod    (было my-pair-2026)
pair_secret     : <64 hex>          (было change-me-...)
relay_host      : 144.31.85.217     (было empty)
relay_port      : 5050
```

### Lua scripts deploy

Phase E файлы скопированы в runtime path `C:\temp\andromeda\scripts\bots\` (где DLL читает при F7 hot-reload):
- `config.lua`, `context.lua`, `fsm.lua`, `states/init.lua` (overwritten)
- `states/meteor_squad.lua`, `states/side_bait.lua` (NEW, 4003B + 3228B)

DLL пересборка не требовалась — pure Lua change. F7 в GUI orchestrator'а перечитает скрипты при следующем запуске.

## Reusable tool: patch_farm_pairing.ps1

`tools/vds_deploy/patch_farm_pairing.ps1` — параметризованный скрипт для патча pairing блока. Идемпотентен (создаёт ts-timestamped backup перед каждым изменением).

Использование на стенде B (после bootstrap):
```powershell
powershell -ExecutionPolicy Bypass -File C:\temp\patch_farm_pairing.ps1 `
  -StandId stand_b `
  -AuthToken 48411ea0bc21bdac3a367ae44bcbb51f `
  -Role slave `
  -PairSecret c3ec1dabbdc707ca9b693b123a0db78709b0e9f2d4f52a40bc11c2caab1cdc4b `
  -RelayHost 144.31.85.217
```

## Что не сделано (блокировано .152 bootstrap)

1. **Stand B (.152) provision** — нужен RDP bootstrap от юзера через `tools/vds_deploy/00_bootstrap_ssh.ps1`. После этого:
   - `scp dotahost:'C:/Users/Administrator/AppData/Local/DotaFarm/*' dotahost2:'C:/Users/Administrator/AppData/Local/DotaFarm/'` — копировать install с A
   - Steam login на B (вручную через RDP, Steam Guard token)
   - `scp tools/vds_deploy/patch_farm_pairing.ps1 dotahost2:'C:/temp/'`
   - Запустить с stand_b/slave параметрами
   - scp Lua → `C:/temp/andromeda/scripts/bots/`
2. **Live verify** — запустить DotaFarm на обоих стендах, проверить:
   - В DotaFarm.log: `[pairing] relay connected user=stand_a pair=two-stand-prod role=master`
   - На сервере (`docker logs dota_relay`): `connect` events для stand_a и stand_b → `pair complete`
   - В orchestrator GUI новая панель "Match Pairing" должна показывать `Connected`
3. **MM smoke**: запустить queue → match found → match_pending_<pid>.json пишутся → MatchPairingFsm broadcast match_found → peer возвращает свой set → ACCEPT/CANCEL decision.

## Status check (без запуска)

Сейчас relay видит 0 active pairs (соответствует, DotaFarm не запущен после patch'а). Можно проверить:
```bash
ssh eft-deploy 'curl -s http://127.0.0.1:5051/health'
# → {"pairs_active":0, ...}

ssh eft-deploy 'curl -s -H "X-Admin-Token: <admin_token>" http://127.0.0.1:5051/admin/stats?user_id=stand_a'
# При connect'е: stats покажут messages_in/out, last_seen_ms
```

## Next

- Юзер: bootstrap .152 через RDP (`00_bootstrap_ssh.ps1`)
- Я: после bootstrap'a — раскатать DotaFarm/Lua/config на .152, запустить E2E

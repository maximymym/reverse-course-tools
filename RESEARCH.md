# Dota 2 Bot Farm — Research Summary

## 1. Античит — VAC3 (usermode, слабый)

- VAC3 usermode only — нет kernel driver, нет Trusted Mode
- External RPM относительно безопасен (VAC ловит DLL inject + known signatures)
- Банволны редкие — скриптеры живут месяцами
- Valve приоритизирует CS2, не Dota 2
- Handle hijacking работает (тот же VAC что в CS2)
- Dota 2 = **64-bit процесс**

## 2. Fog of War — SERVER-SIDE

- Сервер НЕ отправляет позиции врагов в тумане → полный maphack НЕВОЗМОЖЕН
- Для бот-фарма это неважно (мы не читерим, а фармим часы)

## 3. Существующие open-source проекты

### Серьёзные:
- **ExistedGit/Dota2Cheat** — internal C++20, лучший educational
- **Wolf49406/Dota2Patcher** — external, активен, camera hack + VBE
- **LWSS/McDota** — Linux internal, SDK база
- **Panorama JS (Corona API)** — без инжекта, через gameinfo.gi

### Дамперы оффсетов:
- **neverlosecc/source2gen** — ветка `dota` в source2sdk
- **ExistedGit/Dota2Dumper** — специализированный
- **dezlock-dump** — универсальный Source 2

### GC библиотеки:
- **dota2** (Python, steamio) — party, lobby, accept, pick
- **go-dota2** (Go) — поддерживается
- **node-dota2** (Node.js) — deprecated но рабочий

### Auto-accept:
- **gDotaAccepter** — pixel-based
- **dota2autoaccept** — pixel-based

### Bot scripting (Lua, только lobby):
- **Ranked Matchmaking AI** — 3M+ подписчиков
- **OpenHyperAI** — новый бот-скрипт
- **VUL-FT** — full takeover

## 4. Песочница

### Работает:
- VM (Hyper-V/VMware) + GPU passthrough — 1 GPU = 1 VM
- Отдельные ПК
- VPS с GPU (дорого)

### НЕ работает:
- Sandboxie — VAC блокирует matchmaking
- `-master_ipc_name_override` — закрыт Valve сент. 2025
- Docker/WSL2 — нет GPU
- Windows Sandbox — нет persistence

### Hardware per instance:
- RAM: ~3 GB
- CPU: 2 cores
- GPU: нужен реальный GPU (даже на минималках)
- Launch options: `-novid -nojoy -w 640 -h 480 -high`

## 5. Управление Dota 2

### GSI (Game State Integration):
- Read-only, HTTP POST, определяет in-game state
- НЕ определяет: меню, очередь, match found
- Библиотеки: dota2gsipy (Python), Dota2GSI (C#)

### Console commands:
- `dota_player_units_auto_attack_mode 1` — автоатака
- `dota_purchase_item item_tango` — покупка
- Hero pick через консоль НЕ работает
- Accept match через консоль НЕ работает

### Input:
- SendInput/AHK — работает, VAC не банит
- Memory write — мощнее но рискованнее
- Console injection через ICvar — средний вариант

### Matchmaking flow через GC:
- steamio + dota2 — party, invite, accept, pick БЕЗ UI
- Это ключевое преимущество — весь menu flow через protocol

### Private Lobby:
- Steam playtime считается
- Ranked hours НЕ считаются
- Безопасный вариант для чистого фарма Steam часов

## 6. SchemaSystem (идентична CS2)

- source2gen ветка `dota` — полный SDK
- EntitySystem: `GameResourceServiceClient + 0x58 → CGameEntitySystem`
- Chunks по 512, stride 0x70 (как CS2)
- Патчи еженедельно, re-dump = секунды
- Нет pointer encryption, нет обфускации
- ~2000+ классов (больше чем CS2 ~800)

## Sources

### GitHub repos:
- ExistedGit/Dota2Cheat, ExistedGit/Dota2Dumper
- LWSS/McDota, Wolf49406/Dota2Patcher
- neverlosecc/source2gen, neverlosecc/source2sdk (dota branch)
- jm59psut/A--PI (Corona API)
- dougwithseismic/dezlock-dump
- dota2 Python lib (steamio): dota2.readthedocs.io
- paralin/go-dota2
- antonpup/Dota2GSI
- eeacks/SourceEngine-ExecuteCommand-External

### Guides:
- GuidedHacking Dota 2 Section
- Valve Developer Wiki — Dota Bot Scripting
- ModDota Lua Bots API
- danielkrupinski/VAC (disassembled VAC modules)

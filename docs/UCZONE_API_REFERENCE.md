# UC Zone API v2.0 — Reference Card

Собрано 8 параллельными агентами из `https://uczone.gitbook.io/api-v2.0/` + `llms-full.txt` (16389 LOC).
Это чужой коммерческий чит-фреймворк. Мы изучаем его API surface как reference — **enums / callbacks / network / panorama**.
Это НЕ план интеграции, это cheat sheet чтобы понимать экосистему.

---

## 0. Sitemap (структура сайта)

- **Cheats Types and Callbacks** — callbacks, enums, classes/*
  - `classes/math/` — `vector`, `angle`, `vec2`, `vertex`
  - `classes/menu/` — `ctabsection`, `cfirsttab`, `csecondtab`, `cthirdtab`, `cmenugroup`
  - `classes/widgets/` — `cmenuswitch`, `cmenusliderfloat/int`, `cmenubutton`, `cmenucolorpicker(+attachment)`, `cmenucombobox`, `cmenugearattachment`, `cmenuinputbox`, `cmenumulticombobox`, `cmenumultiselect`, `cmenubind`, `cmenulabel`
  - `classes/color`
- **Game Components**
  - `lists/` — abilities, camps, couriers, customentities, entities, heroes, linearprojectiles, modifiers, npcs, physicalitems, players, runes, temptrees, towers, trees
  - `core/` — player, modifier, entity, npc, hero, ability, item, rune, tower, tree, vambrace, camp, bottle, courier, drunkenbrawler, physicalitem, powertreads, tiertoken
  - `game-engine/` — engine, event, gamerules, globalvars, gridnav, input, world, fogofwar, convar
  - `networking-and-apis/` — chatapi, http, steamapi, netchannel, gc, protobuf
  - `rendering-and-visuals/` — particle, renderv1, renderv2, minimap, panorama/panorama, panorama/uipanel
  - `configuration-and-utilities/` — config, humanizer, log, logger, localizer, gamelocalizer, table_ext, stringbuilder, chronos

---

## 1. ENUMS (критичные)

### DOTA_UNIT_ORDER_* (v2026 — актуальные для PrepareUnitOrders)

**Уже подтверждено правильно в нашем `CPanoramaJS.cpp`** (не трогаем):

| OrderType | Name | Наш binding |
|---|---|---|
| 1 | MOVE_TO_POSITION | `OrderMove` ✅ |
| 3 | ATTACK_MOVE | `OrderAttackMove` ✅ |
| 4 | ATTACK_TARGET | `OrderAttackTarget` ✅ |
| 5 | CAST_POSITION | `OrderCastPosition` ✅ |
| 6 | CAST_TARGET | `OrderCastTarget` ✅ |
| 8 | CAST_NO_TARGET | `OrderCastNoTarget` ✅ |
| 11 | TRAIN_ABILITY | `OrderTrainAbility` ✅ |
| 16 | PURCHASE_ITEM | `OrderPurchaseItem` ✅ |
| 21 | STOP | `OrderStop` ✅ |

**Phase 2 — SHIPPED (2026-04-23, DLL `496665c5...`):**

| OrderType | Name | Use case | Lua API |
|---|---|---|---|
| 9 | CAST_TOGGLE | Armlet / Radiance / Soul Ring / Ring of Basilius | `bot:Action_ToggleAbility(ab)` ✅ |
| 20 | CAST_TOGGLE_AUTO | Necro Heartstopper / Sniper Shrapnel autocast | `bot:Action_ToggleAutoCast(ab)` ✅ |
| 30 | VECTOR_TARGET_POSITION | Mirana arrow / Pudge dismember direction | `bot:Action_VectorTargetCast(ab, from, to)` ✅ — payload `{Position, Position2, AbilityIndex}`, fallback `TargetPosition` если spell не реагирует |

**Still TODO (Phase 3 candidates):**

| OrderType | Name | Use case |
|---|---|---|
| 14 | PICKUP_ITEM | Pick up dropped item |
| 15 | PICKUP_RUNE | Bottle filling |
| 19 | MOVE_ITEM | Swap inventory slots |
| 23 | BUYBACK | Buyback after death |
| 24 | GLYPH | Glyph of Fortification |

Полная таблица enum значений:

```
NONE=0
MOVE_TO_POSITION=1
MOVE_TO_TARGET=2
ATTACK_MOVE=3
ATTACK_TARGET=4
CAST_POSITION=5
CAST_TARGET=6
CAST_TARGET_TREE=7
CAST_NO_TARGET=8
CAST_TOGGLE=9              ← Armlet, Radiance, Soul Ring, Ring of Basilius
HOLD_POSITION=10
TRAIN_ABILITY=11
DROP_ITEM=12
GIVE_ITEM=13
PICKUP_ITEM=14
PICKUP_RUNE=15             ← Bottle filling
PURCHASE_ITEM=16
SELL_ITEM=17
DISASSEMBLE_ITEM=18
MOVE_ITEM=19               ← swap inventory slots (у нас было ошибочно 15)
CAST_TOGGLE_AUTO=20        ← Necrolyte Heartstopper autocast, Sniper Shrapnel auto
STOP=21
TAUNT=22
BUYBACK=23
GLYPH=24
EJECT_ITEM_FROM_STASH=25
CAST_RUNE=26               ← Bottle cast rune
PING_ABILITY=27
MOVE_TO_DIRECTION=28
PATROL=29
VECTOR_TARGET_POSITION=30  ← Mirana Arrow vector, Pudge Dismember direction
RADAR=31                   ← SCAN
SET_ITEM_COMBINE_LOCK=32
CONTINUE=33
VECTOR_TARGET_CANCELED=34
CAST_RIVER_PAINT=35
PREGAME_ADJUST_ITEM_ASSIGNMENT=36
DROP_ITEM_AT_FOUNTAIN=37
TAKE_ITEM_FROM_NEUTRAL_ITEM_STASH=38
MOVE_RELATIVE=39
CAST_TOGGLE_ALT=40
CONSUME_ITEM=41
```

### RuneType
```
INVALID=0xFFFFFFFF, DOUBLEDAMAGE=0, HASTE=1, ILLUSION=2, INVISIBILITY=3,
REGENERATION=4, BOUNTY=5, ARCANE=6, WATER=7, XP=8 (=Wisdom), SHIELD=9
```

### ECampType
```
SMALL=0, MEDIUM=1, LARGE=2, ANCIENT=3
```

### CourierState
```
INIT=0xFFFFFFFF, IDLE=0, AT_BASE=1, MOVING=2, DELIVERING_ITEMS=3,
RETURNING_TO_BASE=4, DEAD=5, GOING_TO_SECRET_SHOP=6, AT_SECRET_SHOP=7
```

### DOTA_GAMERULES_STATE_*
```
INIT=0, WAIT_FOR_PLAYERS_TO_LOAD=1, HERO_SELECTION=2, STRATEGY_TIME=3,
PRE_GAME=4, GAME_IN_PROGRESS=5, POST_GAME=6, DISCONNECT=7,
TEAM_SHOWCASE=8, CUSTOM_GAME_SETUP=9, WAIT_FOR_MAP_TO_LOAD=10
```

### TeamType (UC Zone — **относительно local**)
```
TEAM_ENEMY=0, TEAM_FRIEND=1, TEAM_BOTH=2
```
(НЕ путать с `DOTA_TEAM_RADIANT=2 / DIRE=3` Valve enum)

### ModifierState (для HasState checks — **ключевые**)
```
0=ROOTED, 1=DISARMED, 2=ATTACK_IMMUNE, 3=SILENCED, 4=MUTED, 5=STUNNED,
6=HEXED, 7=INVISIBLE, 8=INVULNERABLE, 9=MAGIC_IMMUNE (BKB!),
11=NIGHTMARED, 19=FROZEN, 30=PASSIVES_DISABLED, 32=BLIND,
33=OUT_OF_GAME (Eul/Astral), 36=TRUESIGHT_IMMUNE, 47=FEARED, 48=TAUNTED,
56=DEBUFF_IMMUNE (Lotus)
```

### DamageTypes (битмаска)
```
NONE=0, PHYSICAL=1, MAGICAL=2, PURE=4, HP_REMOVAL=8, ALL=7
```

### UnitTypeFlags (главный classifier, битмаска)
```
HERO=1, CONSIDERED_HERO=2, TOWER=4, STRUCTURE=16, ANCIENT=32,
BARRACKS=64, CREEP=128, COURIER=256, SHOP=512, LANE_CREEP=1024,
WARD=131072, ROSHAN=524288
```

### AbilityBehavior (битмаска, 64-bit, ключевые)
```
NONE=0, HIDDEN=1, PASSIVE=2, NO_TARGET=4, UNIT_TARGET=8, POINT=16, AOE=32,
CHANNELLED=128, ITEM=256, TOGGLE=512, DIRECTIONAL=1024, IMMEDIATE=2048,
AUTOCAST=4096, AURA=65536, ATTACK=131072, ROOT_DISABLES=524288,
VECTOR_TARGETING=1073741824, CAN_SELF_CAST=4294967296
```

### ImmunityTypes (BKB cast filter)
```
NONE=0, ALLIES_YES=1, ALLIES_NO=2, ENEMIES_YES=3, ENEMIES_NO=4,
ALLIES_YES_ENEMIES_NO=5
```

### DispellableTypes
```
NONE=0, YES_STRONG=1 (Aghs dispel), YES=2 (basic dispel), NO=3
```

### AbilityCastResult (диагностика почему не кастуется)
```
READY=-1, NOT_LEARNED=16, NO_MANA=14, ABILITY_CD=15, PASSIVE=17, HIDDEN=60, ITEM_CD=61
```

### PlayerOrderIssuer
```
SELECTED_UNITS=0, CURRENT_UNIT_ONLY=1, HERO_ONLY=2, PASSED_UNIT_ONLY=3
```

### GameMode (ключевые)
```
NONE=0, AP=1, CM=2, RD=3, SD=4, AR=5, ABILITY_DRAFT=18, 1V1MID=21,
ALL_DRAFT=22, TURBO=23, MUTATION=24
```

### Прочие полезные
- **Attributes**: STRENGTH=0, AGILITY=1, INTELLECT=2, ALL=3, MAX=4
- **PingType**: INFO=0, WARNING=1, LOCATION=2, DANGER=3, ATTACK=4, ENEMY_VISION=5, OWN_VISION=6, LIKE=7
- **EKeyEvent**: SCROLL_DOWN=0, SCROLL_UP=1, KEY_DOWN=2, KEY_UP=3
- **AbilityTypes**: BASIC=0, ULTIMATE=1, ATTRIBUTES=2, HIDDEN=3
- **TargetTeam**: NONE=0, FRIENDLY=1, ENEMY=2, BOTH=3, CUSTOM=4
- **TargetType**: HERO=1, CREEP=2, BUILDING=4, COURIER=16, TREE=64, ALL=55
- **TargetFlags**: DEAD=8, MAGIC_IMMUNE_ENEMIES=16, FOW_VISIBLE=128, NO_INVIS=256, NOT_ILLUSIONS=8192, PREFER_ENEMIES=1048576
- **FontWeight**: THIN=100 .. NORMAL=400 .. BOLD=700 .. HEAVY=900
- **DotaChatMessage**: 119 значений для парсинга game chat events (first blood/hero kill/disconnect)
- **GameActivity**: 265 значений ACT_DOTA_* (1500-1764) — для detect cast animation start
- **ParticleAttachment** — для rendering

**Отсутствует в публичной доке:** Lane (TOP/MID/BOT), PlayerSlot, HeroPickState, ItemSlot bounds (в практике: 0-5 inv, 6-8 backpack, 9-14 stash, 15-16 neutral).

---

## 2. CALLBACKS (все `On*` events)

### Lifecycle / frame
- **OnScriptsLoaded** — после загрузки всех скриптов
- **OnUpdate** — каждый game tick, только в игре (~30Hz)
- **OnUpdateEx** — то же + меню
- **OnPreHumanizer** — перед humanizer обработкой
- **OnDraw** — draw frame в игре (ImGui-like через `Renderer.*`)
- **OnFrame** — то же + меню
- **OnGameEnd** — конец матча
- **OnGameRulesStateChange(data)** — смена `Enum.GameState`. **КРИТИЧНО** для detect new match.
- **OnThemeUpdate** — смена UI theme

### Entities
- **OnEntityCreate(entity)** — entity создан (поля могут быть не готовы)
- **OnEntityDestroy(entity)** — entity удалён
- **OnNpcSpawned(npc)** — NPC fully initialized (после OnEntityCreate)
- **OnNpcDying(npc)** — начинает умирать
- **OnSetDormant(npc, type)** — NPC ушёл/пришёл из FoW
- **OnEntityHurt** (unsafe): source, target, ability?, damage
- **OnEntityKilled** (unsafe): source, target, ability?
- **OnUnitInventoryUpdated(npc)**

### Modifiers
- **OnModifierCreate(entity, modifier)** — buff applied
- **OnModifierDestroy(entity, modifier)**
- **OnModifierUpdate(entity, modifier)** — refresh/restack

### Animation
- **OnUnitAnimation** — unit, sequenceVariant, playbackRate, castpoint, type, activity, sequence, sequenceName, lag_compensation_time
- **OnUnitAnimationEnd** — unit, snap
- **OnUnitAddGesture** — npc?, sequenceVariant, playbackRate, fadeIn, fadeOut, slot, activity, sequenceName

### Projectiles (для PvP dodge, не нужно фарм-боту)
- **OnProjectile** — tracking: source, target, ability, moveSpeed, dodgeable, expireTime, launch_tick, target_loc, fullName, name, handle
- **OnProjectileLoc** — point-target projectile
- **OnLinearProjectileCreate** — source, origin, velocity, acceleration, maxSpeed, fowRadius, distance, fullName, handle
- **OnLinearProjectileDestroy** — handle

### Particles
- **OnParticleCreate** — index, entity?, entity_id, attachType, fullName (e.g. `particles/items_fx/radiance.vpcf`), name. **Detect Linkens proc / BKB activate / item effects по fullName filter.**
- **OnParticleUpdate** — index, controlPoint, position
- **OnParticleUpdateFallback**, **OnParticleUpdateEntity** — uncommon
- **OnParticleDestroy** — index, destroyImmediately

### Sound / speech / chat
- **OnStartSound** — source?, hash, guid, seed, **name** (e.g. `Roshan.Death`), position. Block через return false? — нет, только OnSpeak блокируется.
- **OnSpeak** — source?, name. **Return false глушит звук.**
- **OnChatEvent** — type, value, value2, value3, playerid_1..6 (НЕ текстовый чат — это game events kill/dc)
- **OnOverHeadEvent** — player_source?, player_target?, target_npc, value (damage/gold numbers)

### Orders / network — **критичные**
- **OnPrepareUnitOrders(data)** — поля: `player` (CPlayer), `order` (Enum.UnitOrder), `target?` (CEntity), `position` (Vector), `ability?` (CAbility), `orderIssuer` (Enum.PlayerOrderIssuer), `npc` (CNPC), `queue` (bool), `showEffects` (bool). **Return false блокирует ордер.** Modify не подтверждён.
- **OnGCMessage(data)** — `msg_type`, `size`, `binary_buffer_send?`, `binary_buffer_recv?`. Return false блокирует **только send**, recv не блокируется.
- **OnSendNetMessage(data)** — `message_id` (int), `message_name` (e.g. `CUserMsg_SayText2`), `buffer` (lightuserdata raw protobuf), `size`. Return false блокирует.
- **OnPostReceivedNetMessage(data)** — `message_id`, `msg_object` (уже распарсенный). Return false блокирует обработку.

### Input
- **OnKeyEvent** — `key` (Enum.ButtonCode: KEY_F7, MOUSE_LEFT), `event` (Enum.EKeyEvent: KEY_DOWN/UP). **Return false — game не получает.** Raw WM_* (OnWndProc) нет.

### Unsafe
- **OnFireEventClient** — name, event userdata (IGameEventManager)
- **OnEntityHurt/Killed** — требует "unsafe features" в Settings → детект-flag для VAC/OW

---

## 3. NETWORKING (GC / NetMessage / HTTP / Steam / Protobuf / Chat)

### GC (GameCoordinator)
```
GC.SendMessage(msg, msg_type, msg_size) -> nil
GC.GetSteamID() -> string                     // SteamID64 local player
```
Декод: `protobuf.decodeToJSON('CMsgDOTAMatchmakingStatsResponse', msg.binary_buffer_recv, msg.size)`

### NetChannel
```
NetChannel.SendNetMessage(name, json) -> nil
NetChannel.GetLatency([flow]) -> number        // seconds
NetChannel.GetAvgLatency([flow]) -> number
```
Блокировать only через `return false` в OnSendNetMessage/OnPostReceivedNetMessage.

### HTTP
```
HTTP.Request(method, url, [data], callback, [param]) -> boolean
// data: { headers: table, cookies: string, data: string, timeout: number }
// callback(response, code, header, param)
```
Полностью async.

### Steam
```
Steam.SetPersonaName(name) / GetPersonaName() / GetGameLanguage()
Steam.GetProfilePictureBySteamId(id64, [large]) -> texture_handle
Steam.GetProfilePictureByAccountId(account_id, [large])
```
**Нет:** friend list, party invite, lobby join напрямую.

### Protobuf
```
protobuf.encodeFromJSON(name, json) -> table
protobuf.decodeToJSON(name, binary, size) -> string
protobuf.decodeToJSONfromObject(msg_object) -> string
protobuf.decodeToJSONfromString(bytes) -> string
protobuf.free(obj)
```

### Chat
```
Chat.GetChannels() -> string[]                 // team/all/lobby/whisper
Chat.Print(channel, text)                      // локально (не отправляется на сервер)
Chat.Say(channel, text)                        // РЕАЛЬНАЯ отправка
Chat.Flip(channel)
Chat.Roll(channel, [min=0], [max=100])
```
IsAllChat / OnChatEvent — только через `OnPostReceivedNetMessage` + `CUserMessageSayText2`.

---

## 4. PANORAMA JS (единственный entry point)

```
Engine.RunScript(js: string, [contextPanel: "Dashboard" | UIPanel]) -> boolean
Engine.ExecuteCommand(cmd: string) -> nil     // console
```

**UC Zone НЕ wrap'пит `Game.*` / `GameUI.*` / `$.Msg` / `$.Schedule` / `$.RegisterEventHandler`.** Всё — raw JS строки внутри `Engine.RunScript`. Только `Engine.AcceptMatch(state)` / `Engine.CanAcceptMatch()` есть как native wrapper.

### Примеры
```lua
-- Print to Dota console
Engine.RunScript("$.Msg('Hello from Lua!')")

-- Say in chat через console
Engine.ExecuteCommand('say "Hello from Lua!"')

-- Accept match
if Engine.CanAcceptMatch() then Engine.AcceptMatch(0) end

-- Inject button with JS handler (full example в доке)
Engine.RunScript([[
  (function(){
    let ctx = $.GetContextPanel();
    let btn = ctx.FindChildTraverse("my_button");
    btn.SetPanelEvent("onactivate", () => $.Msg("clicked!"));
  })()
]], some_uipanel)

-- Play UI sound
Engine.PlayVol("sounds/npc/courier/courier_acknowledge.vsnd_c", 0.5)

-- Camera to location
Engine.LookAt(x, y)

-- Quick buy
Engine.SetQuickBuy("item_blink", true)
```

### Нативная Lua-сторона для panel tree (вместо $ JS)
```
Panorama.GetPanelByName(id, is_type_name) -> UIPanel|nil
Panorama.GetPanelByPath(path[], [bLogError]) -> UIPanel|nil
Panorama.GetPanelInfo(path[], [bLogError], [useJsFunc]) -> {x, y, w, h}
Panorama.CreatePanel(type, id, parent, classes, styles) -> UIPanel

UIPanel:FindChild(id)
UIPanel:FindChildTraverse(id)
UIPanel:FindChildWithClass(cls)
UIPanel:GetChildByPath(path)
UIPanel:GetParent() / GetRootParent() / GetChildCount() / GetChild(i)
UIPanel:SetVisible(b) / IsVisible()
UIPanel:SetStyle(cssString) / AddClasses(s) / RemoveClasses(s) / HasClass(c)
UIPanel:SetText(s) / GetText() / SetTextType(2|3)    // 2=plain 3=html
UIPanel:GetLayoutWidth/Height / GetXOffset / GetYOffset / GetBounds
UIPanel:BSetProperty(k, v) / SetAttribute(k, v) / GetAttribute(k, default)
```

### JS → Lua bridge
**Нет.** Для event-driven flow используй:
- `Event.AddListener(name)` + `OnFireEventClient(data)` callback (unsafe mode нужен)
- `OnChatEvent(data)`, `OnGCMessage(data)`, `OnSendNetMessage(data)`, `OnPostReceivedNetMessage(data)`

---

## 5. GLOBAL CONTAINERS / FILTERS

Общий паттерн: `Count() / Get(idx) / GetAll([filter]) / Contains(obj)` у **ВСЕХ**.

### Heroes
```
Heroes.Count() / Get / GetAll / Contains
Heroes.GetLocal() -> CHero
Heroes.InRadius(pos, r, teamNum, teamType, [omitIllusions=false], [omitDormant=true])
```

### NPCs (**самый мощный**)
```
NPCs.Count() / Get / GetAll([filter: UnitTypeFlags | fn])
NPCs.InRadius(pos, r, teamNum, teamType, [omitIllusions], [omitDormant])
NPCs.GetInScreen([filter: UnitTypeFlags], [skipDormant=true])
  -> {entity: CNPC, position: Vec2}[]    // готовый ESP iterator
```

### Towers / Couriers / Players / Abilities / Modifiers / PhysicalItems / Entities
```
XXX.Count() / Get / GetAll / Contains
Towers.InRadius(pos, r, teamNum, [teamType])
Couriers.GetLocal()
Players.GetLocal()
Entities.GetAll()       // только GetAll
```

### Camps / Trees / TempTrees
```
Camps.Count() / Get / GetAll / Contains / InRadius(pos, r)    // без team
Trees.InRadius(pos, r, [active=true])                          // active=не срублены
TempTrees.InRadius(pos, r)                                     // Sprout/Iron Branch
```

### Runes / LinearProjectiles
```
Runes.Count() / Get / GetAll / Contains     // без InRadius
LinearProjectiles.GetAll()
  -> { handle, max_speed, max_dist, start_position, position,
       velocity, original_velocity, acceleration, fow_radius, source: CEntity }[]
```

### CustomEntities (multi-unit helpers)
```
CustomEntities.GetSpiritBear(spiritBearAbility) -> CNPC|nil    // Lone Druid
CustomEntities.GetVengeIllusion(commandAura) -> CNPC|nil
CustomEntities.GetTetheredUnit(tether) -> CNPC|nil             // Wisp
CustomEntities.GetTempestDouble(ability) -> CNPC|nil           // Arc Warden
CustomEntities.GetMeepoIndex(dividedWeStand) -> int|nil        // 0-4
```

---

## 6. CORE CLASSES

### Ability
```
GetName / GetLevel / GetMaxLevel / GetCooldown / GetCooldownLength
GetCooldownTimeRemaining / GetManaCost / IsFullyCastable / IsReady
IsBasic / IsUltimate / IsAttributes / IsPassive / IsChannelling
GetBehavior / GetType / GetTargetTeam / GetTargetType / GetTargetFlags
GetDamageType / GetImmunityType / GetDispellableType
GetCastPoint / GetCastRange / GetDamage / GetHealthCost
GetLevelSpecialValueFor(name, [lvl])    // KV value из ability
GetCurrentCharges / ChargeRestoreTimeRemaining / GetToggleState
CastNoTarget(a, [queue, push, fast, id])
CastTarget(a, target, [queue, push, fast, id])
CastPosition(a, pos, [queue, push, fast, id, force_minimap])
Toggle(a, [queue...])                   // Armlet / Radiance / Berserker Call
ToggleMod(a, [queue...])                // autocast
CanBeExecuted() -> AbilityCastResult
```

### NPC (базовый класс + унаследовано Hero/Creep/Tower/etc)
```
GetLocation / GetHealth / GetMaxHealth / GetMana / GetMaxMana / GetTeam / GetLevel
GetUnitName / IsAlive / IsNull
GetMoveSpeed / GetBaseSpeed / GetAttackRange / GetAttackRangeBonus
GetAttackSpeed / GetBaseAttackSpeed / GetAttackProjectileSpeed / GetAttackAnimPoint
GetPhysicalArmorValue / GetPhysicalDamageReduction
GetMagicalArmorValue / GetMagicalArmorDamageMultiplier
GetMinDamage / GetBonusDamage / GetTrueDamage / GetTrueMaximumDamage
HasModifier(name) / GetModifier(name) / GetModifiers([filter]) / GetModifierByIndex(idx)
HasAnyModifier(names) / HasState(state) / GetStatesDuration(states, [active])
IsStunned / IsSilenced / IsDisarmed / IsHexed / IsRooted / IsRunning / IsAttacking
IsLinkensProtected / IsMirrorProtected / HasAegis / IsMagicImmune / IsInvulnerable
IsVisible / IsVisibleToEnemies / IsTurning
IsIllusion / IsCreep / IsLaneCreep / IsStructure / IsTower / IsBarracks
IsAncient / IsRoshan / IsNeutral / IsHero / IsCourier / IsWard / IsMeepoClone
IsRanged / IsConsideredHero / HasScepter / HasShard
HasItem(name, [isReal]) / GetItem(name, [isReal]) / GetItemByIndex(idx)
  // slot 0-5=inv, 6=backpack+, 9=neutral, 15=TP
HasAbility(name) / GetAbility(name) / GetAbilityByIndex(idx) / GetAbilityByActivity(act)
IsEntityInRange(npc, range) / IsPositionInRange(pos, range, [hull])
GetChannellingAbility / IsChannellingAbility
MoveTo(pos, [queue, show, cb, fast, id, fmm])
GetDayTimeVisionRange / GetNightTimeVisionRange
GetBountyXP / GetGoldBountyMin / GetGoldBountyMax
GetHullRadius / GetCollisionPadding / GetTurnRate
GetModifierProperty(prop) / GetBaseSpellAmp
IsInRangeOfShop(shopType, [specific])
```

### Hero (расширение NPC)
```
GetCurrentXP / GetAbilityPoints / GetRespawnTime / GetRespawnTimePenalty
GetPrimaryAttribute / GetStrength / GetAgility / GetIntellect (+Total)
GetLastHurtTime / GetHurtAmount / GetRecentDamage
GetLifeState / GetPlayerID / TalentIsLearned(talent)
GetFacetAbilities / GetFacetID / GetLastVisibleTime
```

### Entity (base — статические методы)
```
Entity.Get(idx) / GetIndex / GetClassName / GetUnitName / GetTeamNum
Entity.GetAbsOrigin / GetRotation / GetRotationPYR
Entity.IsSameTeam(e1, e2) / IsAlive / IsDormant
Entity.GetOwner / RecursiveGetOwner / OwnedBy
Entity.GetHeroesInRadius(e, r, [team, omitIll, omitDorm])
Entity.GetUnitsInRadius(e, r, ...)
Entity.GetTreesInRadius / GetTempTreesInRadius
Entity.IsEntity / IsNPC / IsHero / IsPlayer / IsAbility
```

### Modifier
```
GetName / GetClass / GetSerialNumber / GetStringIndex
GetCreationTime / GetLastAppliedTime / GetDuration / GetDieTime
GetStackCount / IsAura / IsAuraActiveOnDeath / GetAuraSearchTeam
IsCurrentlyInAuraRange / GetAbility / GetAuraOwner / GetParent / GetCaster
GetState -> num, num          // raw state bits
IsDebuff
GetField(name, [dbg])          // raw memory read — fragile cross-patch
```

### Item (наследует Ability)
```
GetCost / GetCurrentCharges / GetSecondaryCharges
GetPurchaseTime / GetAssembledTime / PurchasedWhileDead
IsCombinable / IsPermanent / IsStackable / IsRecipe
IsDroppable / IsPurchasable / IsSellable / IsAlertable
RequiresCharges / IsKillable / IsDisassemblable
GetInitialCharges / CastsOnPickup
CanBeUsedOutOfInventory / IsItemEnabled / GetEnableTime
GetPlayerOwnerID / GetSharability / IsMarkedForSell
GetStockCount(item_id, [team])            // Smoke / Gem / Ward stock
```

### Player
```
Player.PrepareUnitOrders(p, type, target, pos, ability, issuer, issuer_npc,
                         [queue, show_fx, callback, fast, id, fmm])
Player.HoldPosition(p, npc, [queue, push, fast, id])
Player.AttackTarget(p, npc, target, [..., fmm])
Player.GetPlayerID / GetPlayerSlot / GetPlayerTeamSlot
Player.GetName -> str, str
Player.GetProName(steamId) -> str
Player.GetPlayerData([out]) -> table       // kills/deaths/assists/lh/dn
Player.GetTeamData / GetTeamPlayer / GetNeutralStashItems
Player.GetTotalGold(p) -> int
Player.GetAssignedHero / GetActiveAbility
Player.GetSelectedUnits / AddSelectedUnit / ClearSelectedUnits
Player.GetQuickBuyInfo / GetCourierControllerInfo
```

---

## 7. GAMERULES / ENGINE / GLOBALVARS / CONVAR / INPUT / EVENT

### GameRules
```
GameRules.GetServerGameState() -> Enum.GameState     // серверный — trust this
GameRules.GetGameState()                             // клиентский
GameRules.GetGameMode() -> Enum.GameMode
GameRules.GetPreGameStartTime / GetGameStartTime / GetGameEndTime / GetGameLoadTime
GameRules.GetGameTime()                              // main timer от pregame
GameRules.IsPaused() / IsTemporaryDay() / IsTemporaryNight() / IsNightstalkerNight()
GameRules.GetMatchID() -> int
GameRules.GetLobbyID() -> int
GameRules.GetGoodGlyphCD / GetBadGlyphCD / GetGoodScanCD / GetBadScanCD
GameRules.GetGoodScanCharges / GetBadScanCharges
GameRules.GetStockCount(item_id, [team=Radiant])     // obs=42, sentry=188, smoke
GameRules.GetNextCycleTime() -> time, bool           // день/ночь
GameRules.GetDaytimeStart / GetNighttimeStart / GetTimeOfDay
GameRules.IsInBanPhase / GetAllDraftPhase / IsAllDraftPhaseRadiantFirst / GetBannedHeroes
GameRules.GetDOTATime([pregame=false], [negative=false])  // отображаемый таймер
GameRules.GetLobbyObjectJson() -> string             // CSODOTALobby protobuf → JSON
GameRules.GetStateTransitionTime()
```

### Engine
```
Engine.IsInGame / IsInLobby / IsShopOpen
Engine.ExecuteCommand(cmd) / RunScript(js, [panel="Dashboard"])
Engine.CanAcceptMatch / AcceptMatch(state)
Engine.SetQuickBuy(item_name, [reset=true])
Engine.CreateConfig(name, categories[]) / GetCurrentConfigName / SetNewGridConfig
Engine.LookAt(x, y) / PlayVol(sound, [vol]) / ShowDotaWindow
Engine.GetMMR / GetMMRV2                             // V2 нужно на game thread
Engine.GetGameDirectory / GetCheatDirectory / GetLevelName / GetLevelNameShort
Engine.GetBuildVersion / GetUIState
Engine.ConsoleColorPrintf(r, g, b, [a], text)
Engine.GetHeroIDByName / GetHeroNameByID / GetDisplayNameByUnitName
Engine.ReloadScriptSystem
```

### GlobalVars
```
GlobalVars.GetCurTime / GetServerTick / GetIntervalPerTick / GetFrameCount
GlobalVars.GetAbsFrameTime / GetAbsFrameTimeDev
GlobalVars.GetMapName / GetMapGroupName
```

### ConVar
```
ConVar.Find(name) -> CConVar|nil
ConVar.GetString/Int/Float/Bool(convar)
ConVar.SetString/Int/Float/Bool(convar, value)
```

### Input
```
Input.IsKeyDown(KC) / IsKeyDownOnce(KC)
Input.GetCursorPos() -> x, y / GetWorldCursorPos() -> Vector
Input.IsCursorInRect(x, y, w, h) / IsCursorInBounds(x0, y0, x1, y1)
Input.GetNearestUnitToCursor(team, teamType)
Input.GetNearestHeroToCursor(team, teamType)
Input.IsInputCaptured()      // console/chat/shop open
```

### Event
```
Event.AddListener(name)
Event.IsReliable / IsLocal / IsEmpty(event)
Event.GetBool/Int/Uint64/Float/String(event, field)
```

### World / GridNav / FogOfWar
```
World.GetGroundZ(x, y) -> z

GridNav.IsTraversable(pos, flag) -> bool, int
GridNav.BuildPath(start, end, ignoreTrees, npc_map) -> Vector[]
GridNav.IsTraversableFromTo(...)
GridNav.CreateNpcMap(excluded, includeTempTrees, custColl) / ReleaseNpcMap

FogOfWar.IsPointVisible(pos) -> bool
```

---

## 8. RENDER / VECTOR / GEOMETRY

### Render v2 (рекомендованный)
```
Render.FilledRect(start:Vec2, end:Vec2, color:Color, [rounding], [flags])
Render.Rect / RoundedProgressRect / Line / PolyLine
Render.Circle(pos, r, col, [thick], [startDeg], [pct=1], [rounded], [segs=32])
Render.FilledCircle / CircleGradient / DonutChart
Render.Triangle / FilledTriangle / TexturedPoly(vertices, tex, col, [grayscale])
Render.OutlineGradient / Gradient (4 corner)
Render.Shadow / ShadowCircle / ShadowConvexPoly / ShadowNGon
Render.Blur / Logo
Render.PushClip(s, e, [intersect]) / PopClip
Render.StartRotation(angle) / StopRotation
Render.SetGlobalAlpha(a) / ResetGlobalAlpha
Render.LoadFont(name, [flags], [weight]) -> handle
Render.Text(font, size, text, pos:Vec2, color) / TextSize(font, size, text) -> Vec2
Render.LoadImage(path) -> handle / LoadSvg(path, size) / LoadSvgString
Render.Image(handle, pos, size, col, [round], [flags], [uvMin], [uvMax], [grayscale])
Render.ImageCentered / ImageSize

// Render targets (cached layers)
Render.FindOrCreateRT(name, [w], [h]) -> handle
Render.MarkDirtyRT(handle)
Render.RenderRT(callback, handle, pos, color, [scale], [uvMin], [uvMax]) -> bool
Render.ResizeRT(handle, [w], [h])

Render.WorldToScreen(pos:Vector) -> Vec2, bool isVisible
Render.ScreenSize() -> Vec2
```

### Vector class (важные non-Valve методы)
```
Vector(x, y, z)
:Length / :LengthSqr / :Length2D / :Length2DSqr
:Distance(o) / :Distance2D(o) / :DistanceSqr2D(o)
:IsInRange2D(o, range)                      // no sqrt
:Dot / :Dot2D / :Cross
:Normalize / :Normalized / :Scale / :Scaled
:Lerp(b, t) / :LerpInPlace
:Rotate(angle) / :Rotated / :Perpendicular2D
:DirectionTo(other, [dist=1])
:Extend2D(target, distance)                 // self + normalize(target-self)*d
:Extrapolate(direction, scalar)             // velocity prediction
:MoveForward(angle:Angle, distance)
:AngleBetween2D(middle, point3) -> radians
:ClosestToPoint(entities) -> CEntity, distance
:ToScreen() -> Vec2, bool visible
:IsVisible() -> bool
:ToAngle() -> Vector
:SetGroundZ()                               // = World.GetGroundZ(x,y)
// In-place zero-alloc variants: :AddInPlace/:SubInPlace/:MulInPlace/:DivInPlace/:CopyFrom
```

### Angle class
```
Angle(pitch, yaw, roll)
:GetForward() -> Vector
:GetVectors() -> forward, right, up
:Get / :Set / :CopyFrom / :Clone / :IsZero
```

### Vec2 (screen-space, беднее Vector)
```
Vec2(x, y)
:Set / :Get / :CopyFrom / :Clone / :Length / :IsZero
:AddInPlace / :SubInPlace / :MulInPlace / :DivInPlace
```

### Color
```
Color(r, g, b, a) или Color(hex_no_hash)
.r .g .b .a (0-255)
:AsFraction(r,g,b,a) / :AsHsv / :AsHsl / :AsInt
:ToFraction / :ToHsv / :ToHsl / :ToHex / :Unpack / :IsZero
:Lerp(other, weight) / :LerpInPlace / :Grayscale(weight) / :AlphaModulate(alpha)
```

### MiniMap
```
MiniMap.Ping(pos:Vector, type:Enum.PingType)
MiniMap.SendLine(pos:Vector, initial:bool, clientside:bool)
MiniMap.DrawCircle(pos, r, g, b, a, size)
MiniMap.DrawHeroIcon(unitName, pos, [r, g, b, a], [size])
MiniMap.DrawIconByName(iconName, pos, [r, g, b, a], [size])
MiniMap.GetMousePosInWorld() -> Vector
MiniMap.IsCursorOnMinimap() -> bool
MiniMap.GetMinimapToWorld(ScreenX, ScreenY) -> Vector
```

### Math
**Нет UC Zone-специфичного `Math.*` / `Geometry.*`** — используется обычный Lua `math.*` + методы Vector (`:AngleBetween2D`, `:Perpendicular2D`, `:Extend2D`, `:Rotated`, `:MoveForward`, `:ClosestToPoint`). Нет API для PointInCone, RayIntersect, ProjectPointOnLine — руками.

---

## 9. MISC UTILITIES

### Humanizer (встроенный anti-ban!)
```
Humanizer.IsInServerCameraBounds(pos) -> bool        // visible серверу
Humanizer.GetServerCameraPos() -> Vector
Humanizer.GetClientCameraPos() -> Vector
Humanizer.GetServerCursorPos() -> Vector
Humanizer.GetOrderQueue() -> table[]                 // pending orders
Humanizer.IsSafeTarget(entity) -> bool
Humanizer.ForceUserOrderByMinimap()
```

### Logging
```
Log.Write(any)                                        // плоский console log
Logger(name)                                          // typed logger
  :debug(msg) :info(msg) :warning(msg) :error(msg)
  :set_level(Logger.INFO/DEBUG/WARNING/ERROR) :get_level :get_name
// Output: [LEVEL] [LoggerName] message
```

### Config (INI в configs/)
```
Config.ReadInt/Float/String(file, key, [default])
Config.WriteInt/Float/String(file, key, value)
```

### Localization
```
Localizer.Get(str) / RegToken(str)                    // custom cheat strings
GameLocalizer.Find(token)                             // #dota_npc_*
GameLocalizer.FindAbility("antimage_mana_void")
GameLocalizer.FindItem("item_blink")
GameLocalizer.FindNPC("npc_dota_hero_necrolyte")
```

### Table extensions
```
table.IsEmpty/Length/Keys/Values/Sum/Reverse
table.Copy/CopyShallow/KvSwap/Merge/MergeHm/Diff/Sorted
table.Map/Filter/Any/All/Find/RemoveElement
```

### StringBuilder (zero-alloc concat)
```
sb = StringBuilder()
sb:append(...) / sb:appendf(fmt, ...) / sb:clear
```

### Chronos
```
chronos.nanotime() -> seconds                         // monotonic ns precision
```

### Menu (виджеты — для reference, у нас свой ImGui)
```
Menu.Create(firstTab, section, secondTab, thirdTab, [group]) -> tab
tab:Create(label) -> group
group:Switch(name, default) -> CMenuSwitch
group:Slider(label, min, max, def, formatter)
group:ColorPicker / Combo / MultiCombo / MultiSelect
group:Button(label, fn, [toggle], [scale])
group:Bind(label, key) -> CMenuBind          // :IsDown/:IsPressed/:IsToggled
group:InputBox(label, default) / Label(text)
Menu.Find(t1, sec, t2, t3, g, w) / Menu.Opened / Menu.Alpha / Menu.Style(color)
```

### Particle utility (VAC-risk для cheat-visuals — не использовать)
```
Particle.Create(name, [attach], [entity]) -> idx
Particle.SetControlPoint / SetControlPointEnt / SetParticleControlTransform
Particle.SetShouldDraw / Destroy
```

---

## 10. СРАВНЕНИЕ С НАШИМ ANDROMEDA BINDING

### Что у нас ЕСТЬ (в `LuaUnitProxy.cpp` / `CPanoramaJS.cpp` / `LuaStubs.hpp`)
- UnitHandle: `Action_Move/AttackUnit/AttackMove/UseAbility(+OnEntity/OnLocation)`,
  `Action_ToggleAbility/ToggleAutoCast/VectorTargetCast` (Phase 2),
  `GetHealth/MaxHealth/Mana/MaxMana/Location/Team/Level/Gold/UnitName/AssignedLane`,
  `GetNearbyHeroes/LaneCreeps/Towers/NeutralCreeps`, `IsAlive/IsNull`,
  `GetAbilityInSlot/GetItemInSlot/FindItemSlot`, `ActionImmediate_PurchaseItem/Chat`
- AbilityHandle: `IsValid/GetName/GetLevel/GetCooldown(+TimeRemaining)/GetManaCost/IsFullyCastable/IsToggle`
- Globals: `Heroes_InRadius(pos, r, filter)` (Phase 2), `RegisterOnGameStateChange(fn)` (Phase 2),
  `GetHeroLastSeenInfo/GetCourier/GetNeutralCampLocations/GetRoshanLocation/Vector()`
  - **Note:** `GetNeutralCampLocations` C++ stub координаты устарели; Lua override
    через `util/camps.lua` (см. `bot_controller.lua`).

### Что ОТСУТСТВУЕТ (основные gaps)
1. `Action_*` для PickUpRune/MoveItem/Buyback/Glyph (OrderType 14/15/19/23/24)
2. ~~VectorTarget cast (OrderType 30)~~ — DONE Phase 2 ✅
3. `HasModifier(name) / HasState(state) / GetModifiers()` и Modifier class целиком
4. Global containers: ~~`Heroes.InRadius`~~ — DONE Phase 2 ✅, `NPCs.GetAll(filter)`, `Towers.GetAll`, `Camps.InRadius`, `Runes.GetAll`, `CustomEntities.*`
5. Classifiers: `IsIllusion / IsIllusionOrClone / IsLinkensProtected / HasAegis / HasScepter / HasShard / IsRoshan / IsCourier / IsWard / IsNeutral`
6. GameRules: `GetServerGameState / GetMatchID / GetLobbyObjectJson / GetStockCount / GetGoodGlyphCD/ScanCD`
7. Callbacks: ~~`OnGameRulesStateChange`~~ — DONE Phase 2 (`RegisterOnGameStateChange`) ✅, `OnModifierCreate+Destroy / OnNpcSpawned / OnGCMessage / OnPrepareUnitOrders / OnSendNetMessage / OnPostReceivedNetMessage / OnStartSound / OnParticleCreate`
8. Entity classification: `IsEntity/IsNPC/IsHero/IsPlayer/IsAbility`, `RecursiveGetOwner` (illusion→real)
9. GridNav: `BuildPath(start, end, ignoreTrees, npcMap)` — path with creep collision
10. Humanizer: `GetOrderQueue / IsSafeTarget / GetServerCameraPos / IsInServerCameraBounds`
11. HTTP.Request, GC.SendMessage, ConVar.Find/Set, Event.AddListener
12. NPCs.GetInScreen (ESP iterator), Heroes.GetLocal, MiniMap.Ping/SendLine
13. Vector: `Distance2D/IsInRange2D/Extend2D/Extrapolate/Rotated/MoveForward/Perpendicular2D/AngleBetween2D/:ToScreen`
14. Config/Logger/Chronos.nanotime/StringBuilder/table extensions
15. Engine: `SetQuickBuy / CreateConfig / AcceptMatch / RunScript(js, panel) / GetMMR / LookAt(x,y) / GetHeroIDByName`

---

## Источники
- `https://uczone.gitbook.io/api-v2.0/` (весь sitemap)
- `https://uczone.gitbook.io/api-v2.0/llms-full.txt` (16389 LOC полный source)
- `https://uczone.gitbook.io/api-v2.0/sitemap-pages.xml`

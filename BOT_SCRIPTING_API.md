# Dota 2 Bot Scripting API Reference

Собрано из: Valve Developer Wiki, community bot repos (Nostrademous/Dota2-FullOverwrite, furiouspuppy/Dota2_Bots, IanClifer/Clifer-AI-Bot-Script), исходников default ботов.

Все скрипты — Lua. Путь: `dota 2 beta/game/dota/scripts/vscripts/bots/`

---

## 1. Entry Point Functions (определяются скриптом)

Эти функции **вызываются движком** — их нужно определить в своих скриптах:

### Глобальные (team_desires.lua, hero_selection.lua, etc.)
```lua
function Think()                    -- вызывается каждый тик для team-level логики
function UpdateRoshDesire()         -- желание идти на Рошана
function TeamBuyback(nHeroID)       -- решение о байбэке
function ItemPurchaseThink()        -- логика покупки предметов
function AbilityUsageThink()        -- логика использования способностей
function AbilityLevelUpThink()      -- логика прокачки способностей
function CourierUsageThink()        -- логика курьера
function BuybackUsageThink()        -- логика байбэка
function HeroSelection()            -- выбор героя (hero_selection.lua)
```

### Per-hero (bot_<heroname>.lua)
```lua
function Think()                    -- основной тик героя
function MinionThink(hUnit)         -- тик для саммонов/иллюзий (hUnit = handle юнита)
```

### Per-mode (mode_<modename>_generic.lua)
```lua
function GetDesire()                -- float [0..1] — желание войти в этот режим
function OnStart()                  -- вызов при активации режима
function OnEnd()                    -- вызов при деактивации режима
function Think()                    -- тик пока режим активен
```

---

## 2. Global Functions

### Game State
| Function | Returns | Description |
|----------|---------|-------------|
| `DotaTime()` | float | Игровое время (с паузами, отрицательное до 0:00) |
| `GameTime()` | float | Реальное время матча (без учёта пауз) |
| `RealTime()` | float | Реальное системное время |
| `GetGameState()` | int | Состояние игры (GAME_STATE_*) |
| `GetGameMode()` | int | Режим игры (GAMEMODE_*) |
| `GetHeroPickState()` | int | Фаза пика/бана |

### Team & Players
| Function | Returns | Description |
|----------|---------|-------------|
| `GetTeam()` | int | Команда текущего бота (TEAM_RADIANT=2, TEAM_DIRE=3) |
| `GetEnemyTeam()` | int | Вражеская команда |
| `GetTeamPlayers(nTeam)` | table | Массив PlayerID игроков команды |
| `GetTeamMember(nPlayerID)` | handle | Handle героя по PlayerID |
| `IsPlayerBot(nPlayerID)` | bool | Игрок — бот? |
| `IsPlayerInHeroSelectionControl(nPlayerID)` | bool | Бот контролирует выбор героя? |
| `SelectHero(nPlayerID, sHeroName)` | void | Выбрать героя для бота |
| `GetSelectedHeroName(nPlayerID)` | string | Имя выбранного героя |
| `IsHeroAlive(nPlayerID)` | bool | Герой жив? |

### Map & Navigation
| Function | Returns | Description |
|----------|---------|-------------|
| `GetWorldBounds()` | Vector,Vector | min,max координаты карты |
| `GetLaneFrontLocation(nTeam, nLane, fOffset)` | Vector | Позиция фронта лейна |
| `GetLaneFrontAmount(nTeam, nLane, bIgnoreWards)` | float | Прогресс фронта лейна [0..1] |
| `GetLocationAlongLane(nLane, fAmount)` | Vector | Координаты вдоль лейна |
| `GetAmountAlongLane(nLane, vLocation)` | float | Прогресс по лейну для координаты |
| `GetNeutralSpawners()` | table | Массив нейтральных спаунов |
| `GetHeightLevel(vLocation)` | int | Уровень высоты в точке |

### Distance
| Function | Returns | Description |
|----------|---------|-------------|
| `GetUnitToUnitDistance(hUnit1, hUnit2)` | float | Расстояние между юнитами |
| `GetUnitToLocationDistance(hUnit, vLoc)` | float | Расстояние юнит→точка |
| `GetTreeLocation(nTreeID)` | Vector | Позиция дерева |

### Buildings
| Function | Returns | Description |
|----------|---------|-------------|
| `GetTower(nTeam, nTowerID)` | handle | Handle башни (TOWER_*) |
| `GetBarracks(nTeam, nBarracksID)` | handle | Handle бараков |
| `GetAncient(nTeam)` | handle | Handle Древнего |
| `GetShrine(nTeam, nShrineID)` | handle | Handle святилища |
| `GetTowerAttackTarget(hTower)` | handle | Текущая цель башни |

### Runes
| Function | Returns | Description |
|----------|---------|-------------|
| `GetRuneStatus(nRuneID)` | int | Статус руны (RUNE_STATUS_*) |
| `GetRuneType(nRuneID)` | int | Тип руны (RUNE_*) |
| `GetRuneSpawnLocation(nRuneID)` | Vector | Позиция спавна руны |
| `GetRuneTimeSinceSeen(nRuneID)` | float | Время с последнего наблюдения |

### Courier
| Function | Returns | Description |
|----------|---------|-------------|
| `GetCourier(nCourierIdx)` | handle | Handle курьера |
| `GetNumCouriers()` | int | Количество курьеров команды |
| `GetCourierState(hCourier)` | int | Состояние курьера (COURIER_STATE_*) |
| `IsCourierAvailable()` | bool | Курьер свободен? |
| `IsFlyingCourier()` | bool | Курьер — летающий? |

### Items & Economy
| Function | Returns | Description |
|----------|---------|-------------|
| `GetItemCost(sItemName)` | int | Стоимость предмета |
| `GetItemStockCount(sItemName)` | int | Количество в магазине |
| `IsItemPurchasedFromSecretShop(sItemName)` | bool | Предмет из секретного магазина? |
| `IsItemPurchasedFromSideShop(sItemName)` | bool | Предмет из бокового магазина? |

### Combat Estimation
| Function | Returns | Description |
|----------|---------|-------------|
| `FindAoELocation(bEnemies, bHeroes, vLoc, nRadius, nMinCount, fTimeAhead, nTeam)` | {targetloc, count} | Оптимальная точка для AoE |
| `GetHeroLastSeenInfo(nPlayerID)` | table | Последнее известное положение героя |

### Roshan & Glyph
| Function | Returns | Description |
|----------|---------|-------------|
| `GetRoshanKillTime()` | float | Время последнего убийства Рошана |
| `GetGlyphCooldown()` | float | Кулдаун глифа |
| `GetScanCooldown()` | float | Кулдаун скана |

### Lane Desires (team-level)
| Function | Returns | Description |
|----------|---------|-------------|
| `GetPushLaneDesire(nLane)` | float | Желание пушить лейн |
| `GetDefendLaneDesire(nLane)` | float | Желание защищать лейн |
| `GetFarmLaneDesire(nLane)` | float | Желание фармить лейн |
| `GetRoshanDesire()` | float | Желание убивать Рошана |

### Unit Lists
| Function | Returns | Description |
|----------|---------|-------------|
| `GetUnitList(nUnitType)` | table | Массив handle юнитов по типу |

### Misc
| Function | Returns | Description |
|----------|---------|-------------|
| `GetScriptDirectory()` | string | Путь к директории скриптов |
| `RandomInt(nMin, nMax)` | int | Случайное целое |
| `RandomFloat(fMin, fMax)` | float | Случайное дробное |
| `Clamp(fValue, fMin, fMax)` | float | Ограничить значение |
| `RemapVal(fValue, fInMin, fInMax, fOutMin, fOutMax)` | float | Ремап значения |
| `RemapValClamped(...)` | float | Ремап с ограничением |

---

## 3. Bot/Unit Handle Methods

Вызываются на handle бота (GetBot()) или любого юнита.

### Identity & State
| Method | Returns | Description |
|--------|---------|-------------|
| `IsNull()` | bool | Handle невалидный? |
| `GetUnitName()` | string | Имя юнита ("npc_dota_hero_lina") |
| `GetPlayerID()` | int | PlayerID |
| `IsBot()` | bool | Бот (не человек)? |
| `IsAlive()` | bool | Жив? |
| `IsIllusion()` | bool | Иллюзия? |
| `GetTeam()` | int | Команда юнита |
| `GetLevel()` | int | Уровень |
| `GetDifficulty()` | int | Сложность бота (DIFFICULTY_*) |
| `GetActiveMode()` | int | Текущий режим (BOT_MODE_*) |
| `GetActiveModeDesire()` | float | Сила желания текущего режима |
| `GetAssignedLane()` | int | Назначенный лейн |

### Position & Movement
| Method | Returns | Description |
|--------|---------|-------------|
| `GetLocation()` | Vector | Текущая позиция |
| `GetFacing()` | float | Угол направления (0-360) |
| `GetVelocity()` | Vector | Вектор скорости |
| `GetCurrentMovementSpeed()` | float | Текущая скорость |
| `GetFront()` | Vector | Точка перед юнитом |
| `IsFacingLocation(vLoc, fDeg)` | bool | Смотрит ли в сторону точки (±fDeg) |
| `DistanceFromFountain()` | float | Расстояние до фонтана |
| `DistanceFromSecretShop()` | float | Расстояние до секрет шопа |
| `DistanceFromSideShop()` | float | Расстояние до сайд шопа |
| `GetXUnitsTowardsLocation(vLoc, fDist)` | Vector | Точка на расстоянии fDist к vLoc |
| `GetBoundingRadius()` | float | Радиус коллизии |

### Health & Mana
| Method | Returns | Description |
|--------|---------|-------------|
| `GetHealth()` | int | Текущее HP |
| `GetMaxHealth()` | int | Максимальное HP |
| `GetHealthRegen()` | float | Реген HP |
| `GetMana()` | float | Текущая мана |
| `GetMaxMana()` | float | Максимальная мана |
| `GetManaRegen()` | float | Реген маны |

### Combat Stats
| Method | Returns | Description |
|--------|---------|-------------|
| `GetAttackRange()` | float | Дальность атаки |
| `GetAttackDamage()` | int | Урон атаки (средний) |
| `GetBaseDamage()` | int | Базовый урон |
| `GetAttackSpeed()` | float | Скорость атаки |
| `GetSecondsPerAttack()` | float | Секунд между атаками |
| `GetAttackPoint()` | float | Время замаха (attack point) |
| `GetAttackProjectileSpeed()` | float | Скорость снаряда |
| `GetLastAttackTime()` | float | Время последней атаки |
| `GetArmor()` | float | Броня |
| `GetMagicResist()` | float | Магическое сопротивление |
| `GetEvasion()` | float | Уклонение |
| `GetRawOffensivePower()` | float | Грубая атакующая сила |
| `GetOffensivePower()` | float | Атакующая сила (с учётом способностей) |
| `GetEstimatedDamageToTarget(bCurrentlyAvailable, hTarget, fDuration, nDamageType)` | float | Оценка урона по цели за время |
| `GetActualIncomingDamage(fDamage, nDamageType)` | float | Реальный входящий урон (с учётом брони/резиста) |

### Targeting
| Method | Returns | Description |
|--------|---------|-------------|
| `GetTarget()` | handle | Текущая цель бота |
| `GetAttackTarget()` | handle | Текущая цель атаки |
| `SetTarget(hUnit)` | void | Установить цель |

### CC & Status
| Method | Returns | Description |
|--------|---------|-------------|
| `IsStunned()` | bool | Оглушён? |
| `IsRooted()` | bool | Обездвижен? |
| `IsSilenced()` | bool | Заглушен? |
| `IsHexed()` | bool | Хексед? |
| `IsNightmared()` | bool | В Nightmare? |
| `IsDisarmed()` | bool | Обезоружен? |
| `IsBlind()` | bool | Ослеплён? |
| `IsMuted()` | bool | Мутед (предметы)? |
| `IsInvisible()` | bool | Невидим? |
| `IsInvulnerable()` | bool | Неуязвим? |
| `IsMagicImmune()` | bool | Магически иммунен? |
| `IsAttackImmune()` | bool | Иммунен к атакам? |
| `IsChanneling()` | bool | Кастует канал? |
| `IsCastingAbility()` | bool | Кастует способность? |
| `IsUsingAbility()` | bool | Использует способность? |
| `HasSilence(bCurrently)` | bool | Имеет сайленс для врагов? |
| `IsUnableToMiss()` | bool | True Strike? |
| `GetStunDuration(bCurrently)` | float | Длительность стана |
| `GetSlowDuration(bCurrently)` | float | Длительность слоу |

### Damage History
| Method | Returns | Description |
|--------|---------|-------------|
| `WasRecentlyDamagedByHero(hHero, fTime)` | bool | Получал урон от конкретного героя? |
| `WasRecentlyDamagedByAnyHero(fTime)` | bool | Получал урон от любого героя? |
| `WasRecentlyDamagedByCreep(fTime)` | bool | Получал урон от крипов? |
| `WasRecentlyDamagedByTower(fTime)` | bool | Получал урон от башни? |
| `TimeSinceDamagedByAnyHero()` | float | Секунд с последнего урона от героя |
| `TimeSinceDamagedByCreep()` | float | Секунд с последнего урона от крипа |
| `TimeSinceDamagedByTower()` | float | Секунд с последнего урона от башни |

### Nearby Units
| Method | Returns | Description |
|--------|---------|-------------|
| `GetNearbyHeroes(fRadius, bEnemy, nMode)` | table | Герои рядом |
| `GetNearbyCreeps(fRadius, bEnemy)` | table | Крипы рядом |
| `GetNearbyLaneCreeps(fRadius, bEnemy)` | table | Лейн-крипы рядом |
| `GetNearbyNeutralCreeps(fRadius)` | table | Нейтральные крипы |
| `GetNearbyTowers(fRadius, bEnemy)` | table | Башни рядом |
| `GetNearbyBarracks(fRadius, bEnemy)` | table | Бараки рядом |
| `GetNearbyTrees(fRadius)` | table | Деревья рядом |

### Modifiers (Buffs/Debuffs)
| Method | Returns | Description |
|--------|---------|-------------|
| `NumModifiers()` | int | Количество модификаторов |
| `GetModifierName(nIndex)` | string | Имя модификатора по индексу |
| `HasModifier(sModName)` | bool | Есть модификатор? |
| `GetModifierByName(sModName)` | handle | Handle модификатора |
| `GetModifierRemainingDuration(hMod)` | float | Оставшееся время модификатора |
| `GetModifierStackCount(hMod)` | int | Стаки модификатора |

### Economy
| Method | Returns | Description |
|--------|---------|-------------|
| `GetGold()` | int | Текущее золото |
| `GetNetWorth()` | int | Нетворс |
| `GetLastHits()` | int | Ласт-хиты |
| `GetDenies()` | int | Денаи |
| `HasBuyback()` | bool | Есть байбэк? |
| `GetBuybackCooldown()` | float | Кулдаун байбэка |
| `GetBuybackCost()` | int | Стоимость байбэка |
| `GetStashValue()` | int | Стоимость предметов в стэше |
| `GetCourierValue()` | int | Стоимость предметов на курьере |

### Inventory
| Method | Returns | Description |
|--------|---------|-------------|
| `GetItemInSlot(nSlot)` | handle | Предмет в слоте (0-5 инв, 6-8 бэкпак, 9-15 стэш) |
| `FindItemSlot(sItemName)` | int | Слот предмета (-1 если нет) |
| `GetItemSlotType(nSlot)` | int | Тип слота |
| `SetNextItemPurchaseValue(nGold)` | void | Зарезервировать золото |

### Abilities
| Method | Returns | Description |
|--------|---------|-------------|
| `GetAbilityByName(sAbilName)` | handle | Handle способности по имени |
| `GetAbilityInSlot(nSlot)` | handle | Handle способности в слоте |
| `GetAbilityCount()` | int | Количество способностей |
| `GetAbilityPoints()` | int | Неиспользованные очки прокачки |

### Action Queue Info
| Method | Returns | Description |
|--------|---------|-------------|
| `NumQueuedActions()` | int | Количество действий в очереди |

---

## 4. Action Functions

Три варианта для каждого действия:
- `Action_*` — **заменяет** текущую очередь действий
- `ActionPush_*` — **добавляет** в начало очереди (interrupt)
- `ActionQueue_*` — **добавляет** в конец очереди

### Movement
| Action | Parameters | Description |
|--------|-----------|-------------|
| `Action_MoveToLocation(vLoc)` | Vector | Двигаться к точке |
| `Action_MoveToUnit(hUnit)` | handle | Двигаться к юниту |
| `Action_AttackMove(vLoc)` | Vector | Атака-движение к точке |
| `Action_MoveDirectly(vLoc)` | Vector | Двигаться напрямую (без патчфайндинга) |

### Combat
| Action | Parameters | Description |
|--------|-----------|-------------|
| `Action_AttackUnit(hUnit, bOnce)` | handle, bool | Атаковать юнита (bOnce=true — одна атака) |

### Abilities
| Action | Parameters | Description |
|--------|-----------|-------------|
| `Action_UseAbility(hAbility)` | handle | Каст способности (no target) |
| `Action_UseAbilityOnEntity(hAbility, hTarget)` | handle, handle | Каст на цель |
| `Action_UseAbilityOnLocation(hAbility, vLoc)` | handle, Vector | Каст на точку |
| `Action_UseAbilityOnTree(hAbility, nTreeID)` | handle, int | Каст на дерево |

### Items
| Action | Parameters | Description |
|--------|-----------|-------------|
| `Action_PurchaseItem(sItemName)` | string | Купить предмет |
| `Action_PickUpItem(hItem)` | handle | Подобрать предмет |
| `Action_DropItem(hItem, vLoc)` | handle, Vector | Выбросить предмет |
| `Action_PickUpRune(nRuneID)` | int | Подобрать руну |

### Misc Actions
| Action | Parameters | Description |
|--------|-----------|-------------|
| `Action_ClearActions(bStop)` | bool | Очистить очередь действий |

### Immediate Actions (не queue — мгновенно)
| Action | Parameters | Description |
|--------|-----------|-------------|
| `ActionImmediate_Chat(sMsg, bAllChat)` | string, bool | Отправить сообщение в чат |
| `ActionImmediate_Ping(fX, fY, bNormalPing)` | float,float,bool | Пингнуть на карту |
| `ActionImmediate_LevelAbility(sAbilName)` | string | Прокачать способность |
| `ActionImmediate_PurchaseItem(sItemName)` | string | Купить предмет (мгновенно) |
| `ActionImmediate_SellItem(hItem)` | handle | Продать предмет |
| `ActionImmediate_SwapItems(nSlot1, nSlot2)` | int, int | Поменять предметы местами |
| `ActionImmediate_DisassembleItem(hItem)` | handle | Разобрать предмет |
| `ActionImmediate_SetItemCombineLock(hItem, bLock)` | handle, bool | Заблокировать сборку |
| `ActionImmediate_Buyback()` | — | Байбэк |
| `ActionImmediate_Glyph()` | — | Активировать глиф |
| `ActionImmediate_Courier(hCourier, nAction)` | handle, int | Действие курьера (COURIER_ACTION_*) |

---

## 5. Ability Handle Methods

Вызываются на handle способности (от GetAbilityByName и т.д.):

| Method | Returns | Description |
|--------|---------|-------------|
| `GetName()` | string | Имя способности |
| `GetLevel()` | int | Текущий уровень |
| `GetMaxLevel()` | int | Максимальный уровень |
| `CanAbilityBeUpgraded()` | bool | Можно прокачать? |
| `IsFullyCastable()` | bool | Готова к касту (мана + кулдаун + не заглушен)? |
| `IsHidden()` | bool | Скрытая способность? |
| `IsActivated()` | bool | Активирована? |
| `IsTrained()` | bool | Прокачана (level > 0)? |
| `GetCooldownTimeRemaining()` | float | Оставшийся кулдаун |
| `GetCastRange()` | float | Дальность каста |
| `GetManaCost()` | float | Стоимость маны |
| `GetCastPoint()` | float | Время каста (cast point) |
| `GetAbilityDamage()` | float | Урон способности |
| `GetDamageType()` | int | Тип урона (DAMAGE_TYPE_*) |
| `GetBehavior()` | int | Поведение (ABILITY_BEHAVIOR_*) |
| `GetTargetType()` | int | Тип цели (ABILITY_TARGET_TYPE_*) |
| `GetTargetTeam()` | int | Команда цели (ABILITY_TARGET_TEAM_*) |
| `GetTargetFlags()` | int | Флаги цели |
| `GetSpecialValueInt(sKey)` | int | Спец. параметр (int) по ключу |
| `GetSpecialValueFloat(sKey)` | float | Спец. параметр (float) по ключу |
| `GetToggleState()` | bool | Состояние тогла (вкл/выкл) |
| `GetCurrentCharges()` | int | Текущие заряды |
| `GetAutoCastState()` | bool | Автокаст включён? |
| `SetAutoCastState(bOn)` | void | Вкл/выкл автокаст |

---

## 6. Item Handle Methods

Предметы — это тоже Ability handles. Все методы из секции 5 работают. Дополнительно:

| Method | Returns | Description |
|--------|---------|-------------|
| `GetName()` | string | Имя предмета ("item_blink", etc.) |
| `GetCurrentCharges()` | int | Заряды (варды, бутылка, etc.) |
| `IsFullyCastable()` | bool | Готов к использованию? |
| `GetCooldownTimeRemaining()` | float | Кулдаун |
| `GetToggleState()` | bool | Состояние тогла |
| `IsCombineLocked()` | bool | Заблокирована сборка? |

---

## 7. Constants

### BOT_MODE (режимы бота)
```lua
BOT_MODE_NONE           = 0
BOT_MODE_LANING         = 1
BOT_MODE_ATTACK         = 2
BOT_MODE_ROAM           = 3
BOT_MODE_RETREAT        = 4
BOT_MODE_SECRET_SHOP    = 5
BOT_MODE_SIDE_SHOP      = 6
BOT_MODE_PUSH_TOWER_TOP = 7
BOT_MODE_PUSH_TOWER_MID = 8
BOT_MODE_PUSH_TOWER_BOT = 9
BOT_MODE_DEFEND_TOWER_TOP = 10
BOT_MODE_DEFEND_TOWER_MID = 11
BOT_MODE_DEFEND_TOWER_BOT = 12
BOT_MODE_ASSEMBLE       = 13
BOT_MODE_TEAM_ROAM      = 14
BOT_MODE_FARM           = 15
BOT_MODE_DEFEND_ALLY    = 16
BOT_MODE_EVASIVE_MANEUVERS = 17
BOT_MODE_ROSHAN         = 18
BOT_MODE_ITEM           = 19
BOT_MODE_WARD           = 20
BOT_MODE_ATTACKING      = 21
```

### BOT_MODE_DESIRE (сила желания, float 0.0 — 1.0)
```lua
BOT_MODE_DESIRE_NONE      = 0.0
BOT_MODE_DESIRE_VERYLOW   = 0.1
BOT_MODE_DESIRE_LOW       = 0.25
BOT_MODE_DESIRE_MODERATE  = 0.5
BOT_MODE_DESIRE_HIGH      = 0.75
BOT_MODE_DESIRE_VERYHIGH  = 0.9
BOT_MODE_DESIRE_ABSOLUTE  = 1.0
```

### BOT_ACTION_DESIRE
```lua
BOT_ACTION_DESIRE_NONE      = 0.0
BOT_ACTION_DESIRE_VERYLOW   = 0.1
BOT_ACTION_DESIRE_LOW       = 0.25
BOT_ACTION_DESIRE_MODERATE  = 0.5
BOT_ACTION_DESIRE_HIGH      = 0.75
BOT_ACTION_DESIRE_VERYHIGH  = 0.9
BOT_ACTION_DESIRE_ABSOLUTE  = 1.0
```

### GAME_STATE
```lua
GAME_STATE_INIT               = 0
GAME_STATE_WAIT_FOR_PLAYERS   = 1
GAME_STATE_HERO_SELECTION     = 2
GAME_STATE_STRATEGY_TIME      = 3
GAME_STATE_TEAM_SHOWCASE      = 4
GAME_STATE_WAIT_FOR_MAP_TO_LOAD = 5
GAME_STATE_PRE_GAME           = 6
GAME_STATE_GAME_IN_PROGRESS   = 7
GAME_STATE_POST_GAME          = 8
```

### TEAM
```lua
TEAM_RADIANT = 2
TEAM_DIRE    = 3
TEAM_NEUTRAL = 4
```

### LANE
```lua
LANE_NONE = 0
LANE_TOP  = 1
LANE_MID  = 2
LANE_BOT  = 3
```

### TOWER (индексы для GetTower)
```lua
TOWER_TOP_1    = 0
TOWER_TOP_2    = 1
TOWER_TOP_3    = 2
TOWER_MID_1    = 3
TOWER_MID_2    = 4
TOWER_MID_3    = 5
TOWER_BOT_1    = 6
TOWER_BOT_2    = 7
TOWER_BOT_3    = 8
TOWER_BASE_1   = 9
TOWER_BASE_2   = 10
```

### BARRACKS
```lua
BARRACKS_TOP_MELEE    = 0
BARRACKS_TOP_RANGED   = 1
BARRACKS_MID_MELEE    = 2
BARRACKS_MID_RANGED   = 3
BARRACKS_BOT_MELEE    = 4
BARRACKS_BOT_RANGED   = 5
```

### RUNE
```lua
RUNE_POWERUP_1  = 0
RUNE_POWERUP_2  = 1
RUNE_BOUNTY_1   = 2
RUNE_BOUNTY_2   = 3
RUNE_BOUNTY_3   = 4
RUNE_BOUNTY_4   = 5

RUNE_STATUS_UNKNOWN   = 0
RUNE_STATUS_AVAILABLE = 1
RUNE_STATUS_MISSING   = 2
```

### UNIT_LIST (для GetUnitList)
```lua
UNIT_LIST_ALLIED_HEROES    = 0
UNIT_LIST_ENEMY_HEROES     = 1
UNIT_LIST_ALLIED_CREEPS    = 2
UNIT_LIST_ENEMY_CREEPS     = 3
UNIT_LIST_ALLIED_WARDS     = 4
UNIT_LIST_ENEMY_WARDS      = 5
UNIT_LIST_ALLIED_BUILDINGS = 6
UNIT_LIST_ENEMY_BUILDINGS  = 7
UNIT_LIST_NEUTRAL_CREEPS   = 8
UNIT_LIST_ALL              = 9
```

### DAMAGE_TYPE
```lua
DAMAGE_TYPE_PHYSICAL = 0
DAMAGE_TYPE_MAGICAL  = 1
DAMAGE_TYPE_PURE     = 2
DAMAGE_TYPE_ALL      = 3
```

### ABILITY_BEHAVIOR (bitmask)
```lua
ABILITY_BEHAVIOR_HIDDEN          = 1
ABILITY_BEHAVIOR_PASSIVE         = 2
ABILITY_BEHAVIOR_NO_TARGET       = 4
ABILITY_BEHAVIOR_UNIT_TARGET     = 8
ABILITY_BEHAVIOR_POINT           = 16
ABILITY_BEHAVIOR_AOE             = 32
ABILITY_BEHAVIOR_CHANNELLED      = 64
ABILITY_BEHAVIOR_NOT_LEARNABLE   = 128
ABILITY_BEHAVIOR_ITEM            = 256
ABILITY_BEHAVIOR_TOGGLE          = 512
ABILITY_BEHAVIOR_AUTOCAST        = 1024
ABILITY_BEHAVIOR_ATTACK          = 2048
```

### ABILITY_TARGET_TEAM
```lua
ABILITY_TARGET_TEAM_NONE     = 0
ABILITY_TARGET_TEAM_FRIENDLY = 1
ABILITY_TARGET_TEAM_ENEMY    = 2
ABILITY_TARGET_TEAM_BOTH     = 3
ABILITY_TARGET_TEAM_CUSTOM   = 4
```

### ABILITY_TARGET_TYPE
```lua
ABILITY_TARGET_TYPE_NONE    = 0
ABILITY_TARGET_TYPE_HERO    = 1
ABILITY_TARGET_TYPE_CREEP   = 2
ABILITY_TARGET_TYPE_BUILDING = 4
ABILITY_TARGET_TYPE_COURIER = 16
ABILITY_TARGET_TYPE_ALL     = 27
ABILITY_TARGET_TYPE_TREE    = 32
```

### DIFFICULTY
```lua
DIFFICULTY_EASY   = 0
DIFFICULTY_MEDIUM = 1
DIFFICULTY_HARD   = 2
DIFFICULTY_UNFAIR = 3
```

### COURIER_STATE
```lua
COURIER_STATE_IDLE               = 0
COURIER_STATE_AT_BASE            = 1
COURIER_STATE_MOVING             = 2
COURIER_STATE_DELIVERING_ITEMS   = 3
COURIER_STATE_RETURNING_TO_BASE  = 4
COURIER_STATE_DEAD               = 5
```

### COURIER_ACTION (для ActionImmediate_Courier)
```lua
COURIER_ACTION_RETURN            = 0
COURIER_ACTION_SECRET_SHOP       = 1
COURIER_ACTION_TAKE_ITEMS        = 2
COURIER_ACTION_TRANSFER_ITEMS    = 3
COURIER_ACTION_TAKE_AND_TRANSFER = 4
COURIER_ACTION_BURST             = 5
COURIER_ACTION_ENEMY_SECRET_SHOP = 6
COURIER_ACTION_SIDE_SHOP         = 7
COURIER_ACTION_SIDE_SHOP2        = 8
```

### PURCHASE_ITEM (return codes)
```lua
PURCHASE_ITEM_SUCCESS              = 0
PURCHASE_ITEM_OUT_OF_STOCK         = 1
PURCHASE_ITEM_NOT_AT_HOME_SHOP     = 2
PURCHASE_ITEM_NOT_AT_SIDE_SHOP     = 3
PURCHASE_ITEM_NOT_AT_SECRET_SHOP   = 4
PURCHASE_ITEM_NOT_ENOUGH_GOLD      = 5
PURCHASE_ITEM_INVALID_ITEM_NAME    = 6
PURCHASE_ITEM_DISALLOWED_ITEM      = 7
```

### GAMEMODE
```lua
GAMEMODE_AP  = 1   -- All Pick
GAMEMODE_CM  = 2   -- Captain's Mode
GAMEMODE_RD  = 3   -- Random Draft
GAMEMODE_SD  = 4   -- Single Draft
GAMEMODE_AR  = 5   -- All Random
GAMEMODE_INTRO = 6
GAMEMODE_HW  = 7
GAMEMODE_REVERSE_CM = 8
GAMEMODE_XMAS = 9
GAMEMODE_TUTORIAL = 10
GAMEMODE_MO  = 11  -- Mid Only
GAMEMODE_LP  = 12  -- Least Played
GAMEMODE_POOL1 = 13
GAMEMODE_FH  = 14
GAMEMODE_CUSTOM = 15
GAMEMODE_CD  = 16  -- Captain's Draft
GAMEMODE_BD  = 17  -- Balance Draft
GAMEMODE_AD  = 18  -- Ability Draft
GAMEMODE_ARDM = 20 -- All Random Deathmatch
GAMEMODE_1V1MID = 21
GAMEMODE_AP_RANKED = 22
GAMEMODE_TURBO = 23
```

---

## 8. Debug Functions

| Function | Parameters | Description |
|----------|-----------|-------------|
| `DebugDrawText(fX, fY, sText, nR, nG, nB)` | float,float,string,int,int,int | Текст на экране |
| `DebugDrawCircle(vCenter, fRadius, nR, nG, nB)` | Vector,float,int,int,int | Круг на миникарте |
| `DebugDrawLine(vStart, vEnd, nR, nG, nB)` | Vector,Vector,int,int,int | Линия |
| `DebugPause()` | — | Пауза (для отладки) |

---

## 9. Vector

Встроенный тип для 3D-координат:

```lua
local v = Vector(x, y, z)
v.x, v.y, v.z     -- координаты
v + v2             -- сложение
v - v2             -- вычитание
v * scalar         -- умножение на число
v:Length()          -- длина вектора
v:Length2D()        -- длина 2D (XY)
v:Normalized()     -- нормализованный вектор
v:Dot(v2)          -- скалярное произведение
v:Cross(v2)        -- векторное произведение
```

---

## 10. File Structure (стандартная)

```
bots/
  hero_selection.lua         -- HeroSelection()
  team_desires.lua           -- UpdateRoshDesire(), GetPushLaneDesire(lane), ...
  bot_<heroname>.lua         -- Think(), MinionThink()
  ability_item_usage_<hero>.lua  -- AbilityUsageThink(), ItemUsageThink()
  item_purchase_<hero>.lua   -- ItemPurchaseThink() или GetItemsToBuy()
  mode_<mode>_generic.lua    -- GetDesire(), OnStart(), OnEnd(), Think()
```

Движок ищет файлы в `bots/` по конвенции имён. Если файл не найден — используется дефолтный бот.

---

## 11. Полезные паттерны

### Получить ближайшего врага
```lua
local bot = GetBot()
local enemies = bot:GetNearbyHeroes(1600, true, BOT_MODE_NONE)
if #enemies > 0 then
    local closest = enemies[1]  -- уже отсортированы по расстоянию
end
```

### Каст способности на ближайшего врага
```lua
local ability = bot:GetAbilityByName("lina_laguna_blade")
if ability:IsFullyCastable() then
    local target = enemies[1]
    if GetUnitToUnitDistance(bot, target) < ability:GetCastRange() then
        bot:Action_UseAbilityOnEntity(ability, target)
    end
end
```

### Покупка предметов
```lua
function ItemPurchaseThink()
    local bot = GetBot()
    local items = {"item_tango", "item_branches", "item_boots"}
    for _, item in ipairs(items) do
        if bot:FindItemSlot(item) == -1 then
            bot:ActionImmediate_PurchaseItem(item)
            return
        end
    end
end
```

### Прокачка способностей
```lua
function AbilityLevelUpThink()
    local bot = GetBot()
    if bot:GetAbilityPoints() > 0 then
        local order = {"lina_dragon_slave", "lina_light_strike_array",
                       "lina_fiery_soul", "lina_laguna_blade"}
        -- ... логика выбора
        bot:ActionImmediate_LevelAbility(order[1])
    end
end
```

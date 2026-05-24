# Tester Quickstart — правим Lua без пересборки DLL

**Цель этого документа**: за 5 минут объяснить где править скрипты и как применять изменения.

---

## Где лежат файлы

Скрипты ботов живут в **двух** директориях:

```
C:\temp\andromeda\scripts\bots\        ← ЖИВАЯ (DLL читает это)
  ├── bot_controller.lua               ← диспетчер (обычно не трогаешь)
  ├── config.lua                       ← ПАРАМЕТРЫ — сюда лезут чаще всего
  ├── context.lua                      ← pre-compute GameContext (не трогай)
  ├── lane_config.lua                  ← waypoints линий
  ├── item_build.lua                   ← покупки per-hero
  ├── ability_build.lua                ← уровни способностей per-hero
  ├── modes/                           ← отдельные режимы
  │   ├── retreat.lua                  ← убегание (HP thresholds)
  │   ├── lane_farm.lua                ← стандартный лэйнинг
  │   ├── jungle_farm.lua              ← фарм нейтралов
  │   ├── push.lua                     ← толкание башен
  │   ├── gank.lua                     ← роутинг к over-extended врагам
  │   ├── roshan.lua                   ← команда на Рошана
  │   └── hold.lua                     ← дефолт, когда нечего делать
  ├── heroes/                          ← per-hero overrides (опц.)
  │   ├── pudge.lua                    ← пример
  │   └── README.md
  └── util/                            ← helper'ы (Dist2D, TryCastAbility)

~/tools/dota2/scripts_source/bots/      ← SOURCE-OF-TRUTH (git-tracked)
~/tools/dota2/scripts_source/docs/      ← эта документация
```

`scripts_source/` — мастер-копия, живёт в проекте. При `package.sh` копируется в live.
Тестер правит **live** (`C:\temp\andromeda\...`) для быстрого экспериментирования,
разработчик синхронизирует правки обратно в `scripts_source/` для git.

---

## Net (kernel proxy redirect)

Начиная с этого билда DotaFarm использует **WinDivert** (signed kernel driver)
для редиректа всего TCP+UDP трафика Steam/Dota через per-account SOCKS5 прокси.
Это работает **независимо** от того какой TCP API процесс использует (AFD direct,
ws2_32, WinHTTP). Выбранный прокси указывается per-account в `accounts.json`.

В топбаре есть строка `Net: relay tcp=N udp=M | watched X pids | ...` — показывает
сколько пакетов уже завернули. Если строка красная (`Net: kernel redirect ENABLED
but engine not running`):

1. **Проверь что DotaFarm.exe запущен от админа** (требуется для load драйвера).
2. **Defender** может блокировать `WinDivert64.sys` — добавь в exclusions
   путь установки DotaFarm (`C:\Andromeda\DotaFarm\` либо твой install path).
3. **Старый WinDivert уже установлен другим софтом** (NoLag-VPN, OpenVPN GUI с DPI,
   Clash for Windows): остановить ту службу либо переключиться на старую версию.

Если временно нужно отключить — `config\farm.json` → `useKernelRedirect: false`.
Тогда работает только user-mode `ProxyHook.dll` (старое поведение, ловит только
`ws2_32`-вызовы).

---

## Как применить изменения (3 способа)

### 1. Кнопка "Reload Lua" в DotaFarm (рекомендуется для 5 ботов одновременно)

В топбаре `DotaFarm.exe` — кнопка **Reload Lua**. Один клик — все запущенные
`dota2.exe` перезагружают Lua-state одновременно через per-PID flag.

В оркестраторском логе: `Reload Lua: flag sent to N bot(s)`.
В `botbrain.log`: `=== HOT RELOAD ===`.

### 2. F7 (один инстанс)

Фокус на нужное dota2.exe окно → **F7**. Тот бот перезагрузит Lua.

Legacy: F9 всё ещё работает.

### 3. StartFarm заново

Stop Farm → Start Farm. Полный рестарт DLL + Lua.

---

## Первая правка — "hello world"

1. Открой `C:\temp\andromeda\scripts\bots\config.lua`
2. Найди:
   ```lua
   critical_hp = 0.15,   -- <15% → фонтан
   ```
3. Поменяй на `0.50`
4. Сохрани
5. В DotaFarm GUI → **Reload Lua**
6. Через ~1с в `botbrain.log`: `=== HOT RELOAD ===`
7. Боты теперь убегают в фонтан при HP < 50%
8. Верни `0.15`, Reload снова.

---

## Логи

### `C:\temp\andromeda\botbrain.log`
Главный лог. Смотри в конце когда что-то странное.

Маркеры:
- `=== BotBrain Init ===` — старт DLL
- `=== HOT RELOAD ===` — Reload сработал
- `[LUA-ctrl] action=retreat:SAFE` — что делает Lua каждые ~5с
- `[ERROR]` — ошибка в Lua. **Не крашит бота**, C++ fallback продолжит движение.

### `DotaFarm.log` (рядом с exe)
Оркестратор. Запуск/стоп ботов, GSI события, reload flags.

---

## Сломал скрипт?

1. **Dota не крашится**. В DLL есть C++ `ThinkCppFallback` — бот идёт к линии и атакует ближайшего, просто "тупее".
2. В логе: `[ERROR] BotController_Think: <строка>`.
3. Исправь → Reload → ошибка уйдёт.
4. Если совсем запутался — скачай свежий DotaFarm.zip.

**Lua VM не роняет Dota**, максимум конкретный бот зависает в "HOLD" пока не исправишь.

### Симптом: «боты стоят на месте и только один раз двигаются»

Открой `C:\temp\andromeda\botbrain.log` и ищи в LOAD SUMMARY:

```
Modes loaded: 0    ← плохо
bot Think() = NO   ← плохо
```

Означает что в `bot_controller.lua` потерян global alias. В конце файла должен быть:
```lua
Think = BotController_Think
```
Без него DLL не находит entry point, падает на C++ fallback → один `ATTACKMOVE_LANE` и стоп.

**НЕ добавляй `_G.Modes = Modes` — это Valve legacy имя**, наш override ломает Valve
AI scripts и все 5 dota одновременно крашат при match accept.

---

## Куда двинуться дальше

- `ARCHITECTURE.md` — как устроено (~15 минут на понимание всего).
- `API_REFERENCE.md` — полный список функций/методов. Ctrl+F.
- `COOKBOOK.md` — 15 готовых рецептов под частые задачи.

Типичный flow:
- Хочешь поменять поведение → `config.lua`.
- Хочешь новый mode → `COOKBOOK.md` → рецепт 4 → копируешь template → `modes/init.lua`.
- Хочешь per-hero build → `item_build.lua`, добавить ключ.
- Хочешь per-hero уникальную логику → `heroes/<name>.lua`, в `heroes/init.lua` зарегистрировать.

Удачи!

# Bundle G2 — Egezenn/dota2-minify VPK Integration Research

Дата: 2026-05-04. Расширение Bundle G — добавили VPK patching через
[github.com/Egezenn/dota2-minify](https://github.com/Egezenn/dota2-minify)
(community minifier, GPL-3.0, активно поддерживается, 155★, последний коммит
2026-05-01).

## TL;DR

- **Vendor**: `tools/dota2/dota2_minify/` (`git clone --depth 1` от
  `main` 2026-05-04, см. README.md там же для коммита).
- **Wrapper**: `tools/dota2/dota2_minify_wrapper.py` — наш собственный
  тонкий headless CLI поверх их **данных** (mods/<name>/blacklist.txt +
  bin/blank-files/), без зависимости от их DPG GUI.
- **C++ integration**: extension в `DotaMinifier` (`ApplyVpkPatches` /
  `RevertVpkPatches` / `DetectStaleVpkPatches`) через `CreateProcessA` python
  subprocess.
- **Crash recovery**: marker `C:\temp\andromeda\minifier_backup\vpk\.applied`
  + idempotent revert (наш marker в pak'е защищает от удаления чужих модов).
- **Per-bot изоляция: НЕТ** — VPK session-scoped, все 10 ботов делят один
  Dota install. Apply один раз перед launch loop, revert один раз после Stop.

## Решение по архитектуре: subprocess vs port в C++

Принят **subprocess** путь, но НЕ через их `Minify/__main__.py` (это
DearPyGui приложение, требует window context, читает чекбоксы через
`dpg.get_value()`). Вместо этого свой минимальный wrapper который использует
**только данные** их репо:

| Путь | Pro | Contra | Решение |
|---|---|---|---|
| **Запускать `python -m Minify`** через subprocess | весь pipeline их code (decompile, styling.css, xml_mod, scripts) | нужен DPG window, headless невозможен без monkey-patch DPG (хрупко); тянет всю их dependency chain (dearpygui, defusedxml, json-with-comments, playsound3, screeninfo) | ❌ |
| **Monkey-patch DPG до import** + call `build.patcher()` programmatically | их полный pipeline без UI окна | тонкий хак, ломается на их обновлениях; всё ещё нужны все Python deps | ❌ |
| **Свой wrapper над их данными** (blacklist + blank-files + vpk) | минимум deps (`pip install vpk`), robust, прозрачен; покрывает ВСЕ нужные моды (Misc Optimization, Minify Base/Spells/Items, Mute*, Remove Foilage/Pings/Sprays/WeatherEffects, Tree Mod, Dark Terrain — все blacklist-only) | не поддержим styling.css/xml_mod/script.py моды (они требуют Workshop Tools + Source 2 Viewer) | ✅ |

Styling-only моды (Remove Hero Renders, Remove Main Menu Background, Remove
Showcases) **намеренно исключены** — они визуальные, не дают performance
выигрыша, нужный нашей ферме.

## Поддерживаемые presets

В `tools/dota2/dota2_minify_wrapper.py`:

| Preset | Mods (blacklist) | Эффект |
|---|---|---|
| `minimal_visuals` (default) | Misc Optimization, Minify Base Attacks, Minify Spells & Items, Mute Ambient/Default Announcer/Taunt/Voice Line, Remove Foilage/Pings/Sprays/Weather Effects, Tree Mod, Dark Terrain (13 mods) | Максимальный VRAM/CPU выигрыш — ~50-100k blank-targets, pak ~50-150 MB |
| `performance_only` | Misc Optimization, Minify Base Attacks/Spells & Items, Mute Ambient, Remove Weather Effects (5 mods) | Лёгкий preset для слабых машин |
| `audio_mute_only` | Mute Ambient/Default Announcer/Taunt/Voice Line (4 mods) | Только звук → ~18k blanks, pak ~28 MB; полезно для AFK ботов |

## CLI commands

### `apply --preset <name> [--fix-launch-options]`

```
$ python dota2_minify_wrapper.py apply --preset audio_mute_only
[wrapper] steam library: D:\SteamLibrary
[wrapper] pak01_dir.vpk listing: 371476 entries
[wrapper] preset 'audio_mute_only' → 4 mods
[wrapper]   Mute Ambient Sounds: blacklist resolved → 18 files
[wrapper]   Mute Default Announcer: blacklist resolved → 15492 files
[wrapper]   Mute Taunt Sounds: blacklist resolved → 293 files
[wrapper]   Mute Voice Line Sounds: blacklist resolved → 2685 files
[wrapper] total unique blank-targets: 18488
[wrapper] applied 18488 blanks (0 skipped — unknown ext)
[wrapper] applied 0 replacement files
[wrapper] built D:\SteamLibrary\.../dota_minify/pak66_dir.vpk (27.42 MB)
{"ok": true, "preset": "audio_mute_only", ...}
```

### `revert [--cleanup-launch-options]`

Удаляет ТОЛЬКО pak'и с нашим marker файлом `minify_orchestrator.txt`. Чужие
pak'и (от user'ского запуска оригинального GUI Minify, или Valve original)
оставляет нетронутыми и репортит в `foreign_paks_left`.

### `status`

Печатает JSON: applied/preset/our_paks/foreign_paks/dota_minify_dir.

### `list-presets`

Печатает JSON со всеми presets и их mods.

## Файлы куда пишем

`<steam_lib>/steamapps/common/dota 2 beta/game/dota_minify/pak66_dir.vpk`

Это **отдельный gameinfo overlay path** (`-language minify` опция в Steam).
**Не трогаем** оригинальные `dota/pak01_dir.vpk`. Если pak >2GB, vpk пакет
автоматически разделяет на сегменты `pak66_001.vpk`, `pak66_002.vpk` —
revert чистит и сегменты тоже.

## Backup / revert reliability

- **Marker file** в pak (`minify_orchestrator.txt`, JSON со списком модов +
  preset + timestamp + tool/vendor) — гарантирует что revert не тронет чужой
  pak (например созданный пользователем через их оригинальный GUI Minify).
- **VPK marker в C++** (`C:\temp\andromeda\minifier_backup\vpk\.applied`) —
  для crash recovery: если orchestrator упал между apply и revert, на next
  start `DetectStaleVpkPatches` найдёт marker и вызовет `RevertVpkPatches`.
- **Idempotent**: revert без marker → wrapper всё равно сканирует
  `dota_*` directories, удаляет наши pak'и (опираясь на marker внутри pak),
  exit 0.
- **Steam launch options**: `--fix-launch-options` опция добавляет
  `-language minify` через прямой parse `userdata/<id>/config/localconfig.vdf`.
  По умолчанию выключено (orchestrator уже передаёт `-language` через bot
  launch args). `--cleanup-launch-options` симметричен.

## Совместимость

- **С нашим autoexec.cfg / video.txt** — **ортогональны**. autoexec/video.txt
  это per-bot файлы в (Bot)Steam install; VPK — session-scoped game asset
  override. Можно включать оба.
- **С обновлениями Dota** — community-моды от robbyz512 (Misc Optimization
  и т.п.) обновляются с новыми патчами через CI (см. репо). При обновлении
  Dota пользователь может обновить vendor (`git pull` в `dota2_minify/`)
  для свежих blacklists.
- **VAC** — оригинальный README репо: «mods are loaded as overlays via
  `-language` mechanism, VAC не флагает». 155★ + активный репо без массовых
  отчётов о банах подтверждает. Тем не менее — **используем на свой риск**,
  как и оригинальный GUI Minify.

## Per-bot изоляция

**НЕТ** (выбран подход 1 из task spec). 10 ботов на одной машине → один
Steam install → один `dota_minify/pak66_dir.vpk`. `DotaMinifier::ApplyToBot`
для VPK part — no-op (не вызывается для VPK). VPK apply один раз в
`StartFarmThread` перед launch loop, revert один раз в `StopFarm`.

Альтернатива (symlink-based per-bot Dota install) намеренно НЕ
рассматривалась — overkill, ломает Steam Workshop integrity, blowup disk
space.

## Manual smoke procedure

```powershell
# 1. Установить vpk пакет если нет
pip install vpk

# 2. Вызвать wrapper напрямую (любая директория с PowerShell)
cd "C:\Users\aleks\OneDrive\Документы\реверс курс\tools\dota2"
python dota2_minify_wrapper.py list-presets
python dota2_minify_wrapper.py status

# 3. Apply audio-only preset (быстрее — 28 МБ pak)
python dota2_minify_wrapper.py apply --preset audio_mute_only

# 4. Запустить Dota с -language minify (или через DotaFarm.exe фермой)
"C:\Program Files (x86)\Steam\steam.exe" -applaunch 570 -language minify

# 5. В игре — звуки voice/taunt/announcer должны быть muted
# 6. Revert
python dota2_minify_wrapper.py revert

# 7. Validate (если что-то пошло не так): emergency restore через Steam
# steam://validate/570
```

## Risks / known issues

1. **vpk pip package требуется** (`pip install vpk`). Wrapper падает с
   `ModuleNotFoundError: vpk` если не установлен. README в `package.sh`
   output должен это документировать на стороне юзера.
2. **Изменение GUID Dota патча** может ломать community blacklists (старый
   путь больше не существует). Wrapper детектит через `pak01_dir.vpk`
   listing — несуществующие пути игнорируются (не крашит). Но эффект
   сокращается. Решение: периодически обновлять vendor (`git pull`).
3. **Steam должен быть закрыт** перед `--fix-launch-options` (иначе
   localconfig.vdf перезапишется при exit Steam). Orchestrator сначала
   делает `SteamLauncher::KillAllSteam()` в StartFarm — это устраняет
   проблему для нашей ферме.
4. **dota_minify/pak66_dir.vpk persists** при кратковременном Dota patch
   обновлении (Steam не валидирует overlay языковых директорий). Это и хорошо
   (не нужно re-apply каждый день) и плохо (старый preset может содержать
   пути уже несуществующих файлов). Не критично — удалённые пути просто
   ignored игрой.

## Vendor pin

Клонировано 2026-05-04 от `main` HEAD. Чтобы обновить: `cd tools/dota2/dota2_minify
&& git pull --depth 1` (либо удалить и переклонировать).

Лицензия: GPL-3.0. Наш wrapper и интеграция — независимый код, использует
только данные (blacklist.txt + blank-files/), которые сами по себе не
copyrightable assets.

## Files / ссылки

- `tools/dota2/dota2_minify/` — vendor (Egezenn/dota2-minify)
- `tools/dota2/dota2_minify_wrapper.py` — headless CLI
- `tools/dota2/orchestrator/src/dota_minifier.{h,cpp}` — C++ integration
  (методы `ApplyVpkPatches`/`RevertVpkPatches`/`DetectStaleVpkPatches`/
  `RunPython`)
- `tools/dota2/orchestrator/src/dota_minifier_test.cpp` — 3 новых VPK теста
  (DetectStale negative/positive + ApplyDisabled = no-op)
- `tools/dota2/orchestrator/CMakeLists.txt` — post-build copy wrapper в
  `build/Release/scripts/dota2_minify_wrapper.py`
- `tools/dota2/orchestrator/package.sh` — pack `dota2_minify/Minify/{mods,bin/blank-files}`
  + wrapper в `C:\temp\DotaFarm\`
- `tools/dota2/config/farm.json` — секция `minifier.vpk_*`
- `tools/dota2/orchestrator/src/orchestrator.cpp` — Init() detect+revert
  stale VPK; StartFarmThread() apply VPK один раз перед launch loop;
  StopFarm() revert VPK после per-bot RevertAll

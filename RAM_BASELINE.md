# Dota 2 Farm — RAM Reduction Plan

Цель: снизить RAM каждого инстанса `dota2.exe` на **≥40%**. Пользователь поднимает 5+ инстансов в одной party, сейчас ~3-5GB/инстанс → суммарно 15-25GB.

## Baseline (TODO — заполнить после замера от team-lead)

```
Status: AWAITING MEASUREMENT
```

Заполнить когда придёт:

| Process | WS (MB) | Private (MB) | VM (MB) | Handles | Threads |
|---------|---------|--------------|---------|---------|---------|
| dota2.exe       | TBD | TBD | TBD | TBD | TBD |
| steamwebhelper  | TBD | TBD | TBD | TBD | TBD |
| gameoverlayui   | TBD | TBD | TBD | TBD | TBD |
| Total per slot  | TBD | TBD | TBD | TBD | TBD |
| System commit   | TBD MB | | | | |

Конфиг во время замера:
- Renderer (DX11/Vulkan): TBD
- Resolution: 640x480 (из farm.json)
- Launch args: TBD (Steam properties)
- minifier.enabled: TRUE (см. farm.json — applied_vpk_patches, applied_autoexec, applied_video_txt все ON)
- vpk_preset: `minify_aggressive`

## Что уже сделано (audit)

### Launch args (steam_launcher.cpp + dota_minifier.cpp::BuildLaunchArgs)
```
-novid -nojoy -noaafonts -w 640 -h 480 -sw
-novr -no-browser -low -noborder -map dota -threads 2
+fps_max 15 +volume 0
+r_drawparticles 0 +cl_globallight_shadow_mode 0
+dota_cheap_water 1 +r_deferred_height_fog 0
-console
```
- `-no-browser` уже даёт большую экономию (≈150MB chromium).
- `-low` = process priority low (CPU yielding, RAM не трогает).
- `-threads 2` = job system thread cap.

### autoexec.cfg (dota_minifier.cpp::GenerateAutoexec)
Уже стоят:
- fps_max 15 / mat_queue_mode 0 / r_threaded_renderables 0
- mat_picmip 4 / r_lod 4 / mat_viewportscale 0.25
- mat_specular/bumpmap/phong 0
- r_dynamic 0 / r_3dsky 0 / r_drawskybox 0 / r_ssao 0 / mat_postprocess_enable 0
- r_decals 0 / r_drawdecals 0 / dota_ambient_creatures/cloth 0
- volume 0 / voice_enable 0 / snd_mute_losefocus 1

### video.txt (dota_minifier.cpp::GenerateVideoTxt)
Все levels на min: cpu/mem/gpu/gpu_mem/shaderquality = 0. `r_texture_stream_mip_bias 4`. resolutionWidth/Height из конфига.

### VPK strip (dota2_minify_wrapper.py + Egezenn vendor)
Preset `minify_aggressive` режет:
- **Misc Optimization** (Source 1 fluff)
- **Minify Base Attacks** + **Minify Spells & Items** — particles/sounds кастов
- **Mute Ambient/Default Announcer/Taunt/VoiceLine** — звуки
- **Remove Foilage/Pings/Sprays/Weather Effects/River** — терраин
- **Tree Mod** + **Dark Terrain** — replace terrain assets

**ВЫВОД:** это режет в основном **GPU memory + disk streaming**, не процессную RAM. Текстуры героев, models, particle libraries загружены в process memory всё равно через VPK directory. **Это объясняет "как с гуся воды".**

## Что ещё не сделано — карта атаки (приоритет по ROI)

### Тир 0 — INSTANT FREE WIN (no risk, no rebuild)

| # | Approach | Expected ROI | Effort | Status |
|---|----------|--------------|--------|--------|
| 0.1 | `SetProcessWorkingSetSize(hProc, -1, -1)` периодически на каждый dota2.exe из orchestrator (каждые 30s) | 200-500MB WS на инстанс (forces page-out) | Низкий (~30 LOC в orchestrator.cpp) | PENDING |
| 0.2 | Kill `steamwebhelper.exe` ассоциированный с каждым ботом после `client.dll loaded` (бот не нуждается в Steam UI) | 200-400MB на инстанс | Низкий (~20 LOC) | PENDING |
| 0.3 | `r_drawhud 0` + `dota_hud_draw 0` (если Panorama бот не читает) + `cl_drawhud 0` | 50-100MB Panorama JS heap | Низкий (cvar в autoexec) | PENDING — VERIFY бот не читает HUD |

### Тир 1 — autoexec расширение (cvar dive)

| # | Approach | Expected ROI | Notes |
|---|----------|--------------|-------|
| 1.1 | `tv_enable 0` + `demo_recordcommands 0` + `replay_enable 0` | 100-200MB replay buffer | Source 2 имеет |
| 1.2 | `r_texture_stream_pool_size 64` (если cvar существует) | 100-200MB texture streaming cache | Verify |
| 1.3 | `panorama_max_uploader_memory 8` + `panorama_max_uploader_memory_audio 0` | 50-100MB Panorama JS | Verify cvars |
| 1.4 | `dota_particle_fully_skip_distance 1` + `r_drawropes 0` + `r_drawropes_holiday 0` | 20-50MB | Existing minor savings |
| 1.5 | `voice_modenable 0` + `voice_loopback 0` + `mat_clipz 0` | 20-50MB | Voice subsystem skip |

### Тир 2 — VPK whitelist (rebuilds pak66, but reversible)

| # | Approach | Expected ROI | Effort |
|---|----------|--------------|--------|
| 2.1 | Из farm.json.heroes (30 героев) построить blacklist для **остальных 94 героев** — blank vmdl/vmat/vsnd_c пути hero_xxx | 200-400MB hero asset metadata + textures | Mid: новый preset `bot_hero_whitelist` + listing pak01 |
| 2.2 | Все `panorama/images/cosmetics/` → blank (бот без кометик) | 100-300MB cosmetics textures | Mid |
| 2.3 | `scripts/items_game.txt` parsing уже жирный, но не убирается через blank — нужно текстовый edit + repack | 50-100MB | High |

### Тир 3 — Process-level (job objects)

| # | Approach | Expected ROI | Effort |
|---|----------|--------------|--------|
| 3.1 | JobObject с `JOBOBJECT_EXTENDED_LIMIT_INFORMATION.ProcessMemoryLimit = 2GB` на каждый dota2.exe | Жёсткий cap (если превышено — OS swap или kill) | Risky — может крашить под нагрузкой |
| 3.2 | `SetProcessAffinityMask` 2 cores per instance | RAM не уменьшает, но reduces sched overhead | Low — already `-threads 2` |
| 3.3 | `SetPriorityClass(IDLE_PRIORITY_CLASS)` (сильнее `-low`) | Less paging contention | Low |

### Тир 4 — D3D hook (apply_d3d_hook сейчас FALSE)

В DLL есть код `apply_d3d_hook` (см. farm.json) — отключает реальный submit во время offscreen → GPU buffers не выделяются под backbuffer. Включить и измерить.

### Тир 5 — Last resort

- `dxvk` + custom shader cache shared between instances → меньше per-instance shader RAM.
- Direct executable patching: strip unused subsystems из dota2.exe (Replay, VOIP, Workshop) — VAC risk, **не делаем**.

## План работы

1. **Замерить baseline** (ждём от team-lead).
2. Тир 0.1 (working set trim) — пишу прямо сейчас в orchestrator.cpp.
3. Тир 0.2 (kill steamwebhelper) — после.
4. Тир 1 (autoexec extras) — добавляю и проверяю что cvars не "unknown command".
5. Замер #2.
6. Если <40% — Тир 2.1 (hero whitelist VPK).
7. Замер #3.
8. Тир 0.3 + Тир 4 (D3D hook) — если нужно ещё.

## Не делаем без явного approval

- Бинарный патч dota2.exe (VAC).
- `-insecure` (VAC).
- Удаление целых системных VPK (краш).
- HWID/sandbox изменения (вне scope этой задачи).

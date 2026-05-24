# Dota 2 Minifier — Research

Цель: уменьшить per-instance ресурсопотребление (CPU/GPU/RAM) Dota 2 в self-play
ферме (10 параллельных dota2.exe). Все изменения должны быть **полностью
обратимы** при `StopFarm` либо при следующем старте orchestrator (если предыдущий
crashed без revert).

Дата: 2026-05-04. Тестировал community-методы из проверенных источников
(2024-2025 материалы), VAC-safety из Valve official policy.

---

## 1. Methods inventory

| Method | Resource savings | VAC-risk | Implementation effort | Reversible | Decision |
|---|---|---|---|---|---|
| **Launch options** (`-novid -nojoy -dx9 -high -map dota -novr -threads 2 -noaafonts`) | 15-30% RAM, faster startup, lower idle CPU | **Safe** (Valve sanctioned) | Trivial — string в `BuildDotaLaunchArgs` | Trivial — не пишем на диск | **CHOSEN** |
| **autoexec.cfg** в `<botSteam>/steamapps/common/dota 2 beta/game/dota/cfg/autoexec.cfg` (`fps_max 30`, `r_drawparticles 0`, `dota_cheap_water 1`, `mat_viewportscale 0.5`, `cl_globallight_shadow_mode 0`, `r_deferred_*` off, `cl_particle_fallback_*`) | 30-50% GPU, 10-20% RAM | **Safe** (legit console commands per dotainternational.com) | Низкий — write file + backup | Высокий — backup → restore | **CHOSEN** |
| **video.txt** в `userdata/<id>/570/local/cfg/video.txt` (`setting.cpu_level=1`, `setting.gpu_level=0`, `setting.gpu_mem_level=0`, `setting.shaderquality=0`, `setting.r_*=0`, `setting.dota_cheap_water=1`, `setting.mat_viewportscale=0.5`, `setting.dota_portrait_animate=0`, resolution 800×600) | 40-60% GPU+VRAM, 20% RAM | **Safe** (UI-equivalent settings) | Средний — нужен Steam userId path resolution | Высокий — backup → restore. Caveat: 2024-02 update в Win11 ломает некоторые keys (известный issue ValveSoftware/Dota2-Gameplay#15594), но большинство всё ещё применяются | **CHOSEN** |
| **VPK low-quality model/texture mods** (1×1 px textures, empty meshes) | 30-50% VRAM | **Risky** — file integrity check может triggernуть VAC | Высокий — собирать VPK, replace `pak01_dir.vpk` | Средний — backup VPK | **REJECTED** (флаг есть в config, реализация только safe-path) |
| **D3D Present hook** (skip rendering вообще) | 70-90% GPU | **High risk** — DLL inject + integrity violations | Высокий — Andromeda hook + frame-skip logic | Не нужен (hook unloads с DLL) | **REJECTED** (флаг есть в config, не реализовано) |
| **Network tweaks** (`rate 80000`, `cl_updaterate 30`, `cl_cmdrate 30`) | Minor — 5-10% network throughput | Safe | Низкий — в autoexec.cfg | Reversible (backup) | **INCLUDED в autoexec.cfg** |
| **Process priority** (`SetPriorityClass`/`-high`) | Helpful только для master/critical bots | Safe | Trivial — launch option | N/A | **INCLUDED в launch options** |
| **Steam Overlay disable** (`-no-browser` / disable in Steam settings) | 30-100 MB RAM per instance | Safe | Trivial — launch option | N/A | **INCLUDED в launch options** |

---

## 2. Recommended stack (CHOSEN)

### 2.1 Launch options (передаются в `dota2.exe` cmd line)

```
-novid              # skip Valve intro
-nojoy              # disable joystick subsystem
-noaafonts          # disable AA on fonts (-AA cost CPU)
-novr               # disable VR module (~50 MB RAM saved)
-map dota           # preload map → faster first match
-dx9                # force DirectX 9 (lower memory footprint vs DX11/Vulkan)
-threads <N>        # cap engine threads (2 для farm-bot, не отбираем CPU у других)
-no-browser         # disable Chromium/CEF (Steam overlay) → -100 MB RAM
-w <W> -h <H>       # forced resolution (800×600 default)
-sw                 # bordered windowed (для tiler)
-console            # console enabled (полезно для diag)
+fps_max <N>        # 30 default (60 для visual debug)
+r_drawparticles 0  # отключить partons (GPU exception)
+cl_globallight_shadow_mode 0
+dota_cheap_water 1
+r_deferred_height_fog 0
```

Текущий `BuildDotaLaunchArgs` (`steam_launcher.cpp:212-232`) уже частично это делает.
Minifier формирует **дополнительные** options поверх базовых; merge через
`AppendIfMissing` (не дублируем `-novid` если он уже там).

**Ожидаемый эффект:** -150 MB RAM per instance (Chromium/VR/intro), -10% startup time.

### 2.2 autoexec.cfg

Путь: `<dotaInstallDir>/game/dota/cfg/autoexec.cfg` (Source 2 location, 2025).
**ВАЖНО**: НЕ путь `dota 2 beta/dota/cfg/` (старый Source 1 — больше не работает
для Source 2 Reborn).

Но в нашей ферме каждый бот имеет свой `BotDota\<idx>\` (junction на assets +
hardlink на dota2.exe), поэтому пишем в:
`<botDir>/game/dota/cfg/autoexec.cfg`.

Содержимое (proven low-quality preset):

```cfg
// === DotaFarm minifier autoexec — auto-generated ===
fps_max 30
r_drawparticles 0
mat_viewportscale 0.5
cl_globallight_shadow_mode 0
dota_cheap_water 1
r_deferred_height_fog 0
r_deferred_simple_light 0
r_ssao 0
r_dota_fxaa 0
r_deferred_specular 0
r_deferred_specular_bloom 0
dota_portrait_animate 0
r_dota_normal_maps 0
r_grass_quality 0
dota_ambient_creatures 0
dota_ambient_cloth 0
r_dota_allow_wind_on_trees 0
r_dashboard_render_quality 0
cl_particle_fallback_base 4
cl_particle_fallback_multiplier 0
voice_enable 0
rate 80000
cl_updaterate 30
cl_cmdrate 30
cl_interp 0
```

**Ожидаемый эффект:** -30-50% GPU usage, -20% RAM (через particle/shadow/SSAO off).

### 2.3 video.txt

Путь: `userdata/<steamId32>/570/local/cfg/video.txt` (per-Steam-user).
Format: VDF (Valve KeyValues) с двумя стилями кавычек.

Steam ID 32 = `steamId64 - 76561197960265728` (lower 32 bits). Получаем из
`AccountConfig.steamId` (он у нас уже 64-bit).

`userdata` directory: для main Steam = `<steamInstall>/userdata`. Для per-bot
Steam (`C:\BotSteam\<idx>\`) — `C:\BotSteam\<idx>\userdata` (если существует —
использовать, иначе fallback на main).

Содержимое (low-quality preset):

```
"VideoConfig"
{
  "setting.cpu_level"                       "1"
  "setting.mem_level"                       "0"
  "setting.gpu_level"                       "0"
  "setting.gpu_mem_level"                   "0"
  "setting.shaderquality"                   "0"
  "setting.mat_vsync"                       "0"
  "setting.dota_cheap_water"                "1"
  "setting.r_deferred_height_fog"           "0"
  "setting.r_deferred_simple_light"         "0"
  "setting.r_ssao"                          "0"
  "setting.r_dota_fxaa"                     "0"
  "setting.r_deferred_specular"             "0"
  "setting.r_deferred_specular_bloom"       "0"
  "setting.dota_portrait_animate"           "0"
  "setting.r_dota_normal_maps"              "0"
  "setting.r_texture_stream_mip_bias"       "2"
  "setting.r_grass_quality"                 "0"
  "setting.dota_ambient_creatures"          "0"
  "setting.dota_ambient_cloth"              "0"
  "setting.r_dota_allow_wind_on_trees"      "0"
  "setting.r_dashboard_render_quality"      "0"
  "setting.cl_particle_fallback_base"       "4"
  "setting.cl_particle_fallback_multiplier" "0"
  "setting.mat_viewportscale"               "0.5"
  "setting.fullscreen"                      "0"
  "setting.defaultres"                      "800"
  "setting.defaultresheight"                "600"
  "setting.aspectratiomode"                 "0"
  "setting.nowindowborder"                  "0"
}
```

**Caveat 2024-02:** на Windows 11 некоторые keys могут не применяться после
in-game UI overrides (issue Dota2-Gameplay#15594). Workaround: ставим
read-only flag после write, чтобы Dota не перезаписала на shutdown.
Backup сохраняем ОРИГИНАЛЬНЫЕ permissions, restore возвращает.

**Ожидаемый эффект:** -40-60% GPU+VRAM (главный выигрыш — `gpu_level=0` +
`shaderquality=0` + viewport 0.5x + texture mip_bias 2).

---

## 3. Optional aggressive (НЕ реализовано — флаги есть в config)

### 3.1 VPK patching (`apply_vpk_patches`)

Заменить `<dotaInstallDir>/game/dota/pak01_dir.vpk` (или нужный slice) на mod
с 1×1 px textures и empty model meshes. Эффект -50% VRAM, но:
- VAC проверяет integrity критичных VPK через chunked SHA hashes
- Любая модификация может вызвать "VAC could not verify game session" при
  matchmaking entry → instance disabled на сессию

**Решение:** оставлен флаг в `MinifierConfig::applyVpkPatches`, реализация —
только если эмпирически подтвердим что в self-play лобби VAC integrity check
скипает или мягче (TBD).

### 3.2 D3D Present hook (`apply_d3d_hook`)

Andromeda DLL (наша) уже инжектится в dota2.exe — добавить hook на
`IDXGISwapChain::Present` → ранний return когда minifier active. Эффект -90%
GPU. Но:
- Если детектится через checksum (Andromeda DLL уже под VAC scrutiny) —
  bann instances
- Headless mode может ломать GSI (Game State Integration) если он зависит от
  render tick

**Решение:** оставлен флаг `MinifierConfig::applyD3dHook`, реализация — после
эксперимента с `headless` mode на тестовом аккаунте.

---

## 4. Crash recovery design

### Backup layout

```
C:\temp\andromeda\minifier_backup\<botIdx>\
  ├── .applied                          # marker: timestamp + sha of source files
  ├── autoexec.cfg.orig                 # original (or .missing если не было файла)
  ├── autoexec.cfg.path                 # absolute path где writable copy лежит
  ├── video.txt.orig
  ├── video.txt.path
  ├── launch_args.orig                  # JSON: исходный launchArgs из FarmConfig
```

### Apply

```cpp
bool ApplyToBot(botIdx, steamId)
{
  if (m_backups[botIdx].valid) return true;  // already applied
  
  BackupState bs;
  bs.backedUpAtMs = NowMs();
  
  // 1. Backup current files (or mark as ".missing")
  if (cfg.applyAutoexec) BackupFile(autoexecPath, bs);
  if (cfg.applyVideoTxt) BackupFile(videoTxtPath, bs);
  // Launch args backed up в memory (он изначально из FarmConfig в RAM)
  
  // 2. Write marker FIRST — если crash после write_marker но до apply,
  //    next start увидит marker, попытается revert (no-op для unmodified).
  WriteMarker(bs);
  
  // 3. Apply minified content
  if (cfg.applyAutoexec) WriteFile(autoexecPath, GenerateAutoexec(cfg));
  if (cfg.applyVideoTxt) WriteFile(videoTxtPath, GenerateVideoTxt(cfg, steamId));
  
  bs.valid = true;
  m_backups[botIdx] = bs;
  return true;
}
```

### Revert

```cpp
bool RevertBot(botIdx)
{
  auto& bs = m_backups[botIdx];
  if (!bs.valid) return true;
  
  // Restore files (or delete если backup был .missing)
  for (auto& [path, content] : bs.savedFiles) {
    if (content == "<MISSING>") DeleteFileA(path.c_str());
    else WriteFile(path, content);
  }
  
  // Remove marker LAST — если crash до удаления marker'а, next start
  // снова попробует revert (idempotent — savedFiles совпадают с current).
  DeleteFileA((backupDir + "\\.applied").c_str());
  
  bs.valid = false;
  return true;
}
```

### DetectStaleBackups (called from Init)

```cpp
bool DetectStaleBackups() {
  // Сканирует C:\temp\andromeda\minifier_backup\*\.applied
  // Если найден — load backup, попытаться revert.
  // Это покрывает случай: orchestrator killed/crashed без StopFarm,
  // dota2.exe тоже завершилась → файлы остались минифицированные.
}
```

### Concurrency

- Каждый бот изолирован: `BotSteam\<idx>\steamapps\common\dota 2 beta\game\dota\cfg\autoexec.cfg`
  и `BotSteam\<idx>\userdata\<id>\570\local\cfg\video.txt` — different paths
- Если несколько ботов sharing main Steam install — Apply делает первый,
  остальные пропускают (лог "shared cfg already minified by bot N").
- BackupState[botIdx] mutex-protected (std::mutex m_mutex)

---

## 5. References

- [Best Dota 2 Launch Options 2025 (Pickem Mongolia)](https://pickem-mongolia.com/news/best-dota-2-launch-options/) — `-novid -nojoy -nod3d9ex -map dota -novr -high +fps_max 60`
- [Dota 2 Launch Options Detailed Guide (Hawk Live)](https://hawk.live/posts/dota-2-launch-options) — explanations of `-map dota`, `-novr`
- [Dota 2 video.txt low-quality (v3rlain3 GitHub)](https://github.com/v3rlain3/dota2.cfg/blob/master/video.txt) — full video.txt low-quality config
- [Dota 2 autoexec.cfg (larrybotha GitHub)](https://github.com/larrybotha/dota2-autoexec/blob/master/autoexec.cfg) — `fps_max`, `mat_*`, `cl_*` reference
- [Autoexec.cfg Steam Community Guide (468214845)](https://steamcommunity.com/sharedfiles/filedetails/?id=468214845) — Source 2 autoexec.cfg path: `dota 2 beta/game/dota/cfg/`
- [What is Dota 2 Console and Autoexec.cfg (DotaInternational)](https://www.dotainternational.com/what-is-dota-2-console-and-autoexec-cfg-best-commands) — VAC safety: "legit console commands (HUD, FPS, performance, basic binds) are safe"
- [Dota 2 video.txt issue 15594 (ValveSoftware GitHub)](https://github.com/ValveSoftware/Dota2-Gameplay/issues/15594) — known 2024-02 bug, video.txt не всегда применяется на Win11
- [Dota 2 PCGamingWiki](https://www.pcgamingwiki.com/wiki/Dota_2) — Steam userdata path: `userdata/<id>/570/local/cfg`
- [VAC ban scope (Liquipedia)](https://liquipedia.net/dota2/Ban) — VAC bans охватывают cheats/scripts/hacks; settings tweaks НЕ являются триггером

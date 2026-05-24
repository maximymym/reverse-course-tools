# Portable Distribution Audit — Bundle H

## Цель
Клиент скачивает zip → unzip в любую папку → запуск `DotaFarm.exe`. Никаких
`pip install`, `winget install Python`, `vc_redist.x64.exe`. Zero install.

## Сводка по runtime dependencies

| Артефакт | До Bundle H | После Bundle H | Action |
|---|---|---|---|
| `DotaFarm.exe` | `/MD` → требует MSVCP140/VCRUNTIME140/api-ms-win-crt-* | `/MT` → static-linked | CMakeLists.txt: `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` (CMP0091 NEW) перед `project()` |
| `Andromeda-Dota2-Base.dll` | уже `/MT` (vcxproj `<RuntimeLibrary>MultiThreaded</RuntimeLibrary>`) | без изменений | dumpbin show: только KERNEL32/USER32/GDI32/SHELL32/ole32/dbghelp/IMM32/D3DCOMPILER_47 (всё OS-shipped) |
| `ProxyHook.dll` | `/MD` → MSVCP140 + VCRUNTIME140 + api-ms-win-crt-* | `/MT` (наследуется через CMP0091 NEW + add_subdirectory) | confirmed после rebuild |
| `ProxyDivert` (static lib) | `/MD` | `/MT` | наследуется |
| `ProxyRelay` (static lib) | `/MD` | `/MT` | наследуется |
| `dota2_minify_wrapper.py` | требует Python 3.8+, `pip install vpk vdf` | PyInstaller bundle → `dota2_minify_wrapper.exe` (single-file, ~10-25 MB) | `dota2_minify_wrapper_build/wrapper.spec` + build instruction в README.md |
| `handle64.exe` (Sysinternals) | external download, non-redistributable EULA | НЕ в bundle. `DotaLauncher::KillMutexNative` (NtQuerySystemInformation primary path) уже работает | code already supports native fallback, log warning переименован в INFO |
| `HwidSpoofer.exe` | external Python+PyInstaller bundle (нет в текущей сборке) | optional. `tools/spoofer/orchestrator/HwidSpoofer.spec` уже существует, но dist/ пустой | сборка спuфера — отдельный сценарий за пределами Bundle H, по умолчанию выключен (`spooferEnabled=false` в FarmConfig) |
| `WinDivert.dll`, `WinDivert64.sys` | vendored, OS-loaded | без изменений | already in bundle |
| `sing-box.exe`, `wintun.dll` | vendored | без изменений | already in bundle |

## DotaFarm.exe dumpbin BEFORE Bundle H

```
d3d11.dll, ADVAPI32.dll, WINHTTP.dll, bcrypt.dll, WS2_32.dll, IPHLPAPI.DLL,
WinDivert.dll, KERNEL32.dll, USER32.dll, COMDLG32.dll,
MSVCP140.dll,                      ← VC++ Redist
ntdll.dll, IMM32.dll, D3DCOMPILER_47.dll,
VCRUNTIME140.dll,                  ← VC++ Redist
VCRUNTIME140_1.dll,                ← VC++ Redist
api-ms-win-crt-runtime-l1-1-0.dll,    ← UCRT (Win10+ shipped, но не на старых Win8/7)
api-ms-win-crt-stdio-l1-1-0.dll,
api-ms-win-crt-heap-l1-1-0.dll,
api-ms-win-crt-string-l1-1-0.dll,
api-ms-win-crt-filesystem-l1-1-0.dll,
api-ms-win-crt-convert-l1-1-0.dll,
api-ms-win-crt-time-l1-1-0.dll,
api-ms-win-crt-locale-l1-1-0.dll,
api-ms-win-crt-math-l1-1-0.dll,
api-ms-win-crt-utility-l1-1-0.dll
```

## DotaFarm.exe dumpbin AFTER Bundle H — ожидаемое

Должны исчезнуть: `MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, все
`api-ms-win-crt-*`. Останутся только OS-shipped DLL (KERNEL32/USER32/ADVAPI32/
ntdll/d3d11/dxgi/dwmapi/winhttp/bcrypt/ws2_32/iphlpapi/IMM32/D3DCOMPILER_47/
COMDLG32/WinDivert).

Размер `.exe` вырастет на ~1-2 MB (статический CRT).

(Verify запускается после rebuild — см. конец документа.)

## Andromeda DLL dumpbin (already clean)

```
KERNEL32.dll, USER32.dll, GDI32.dll, SHELL32.dll, ole32.dll, dbghelp.dll,
IMM32.dll, D3DCOMPILER_47.dll
```

Ноль VC runtime DLL — `<RuntimeLibrary>MultiThreaded</RuntimeLibrary>` в Release
config vcxproj уже выставлен правильно. Action: НЕ ТРОГАТЬ.

## handle64.exe — план

`handle64.exe` от Sysinternals имеет EULA, запрещающую redistribution в bundle.
Альтернативы:

1. **Native NtQuerySystemInformation** — уже реализовано (`dota_launcher.cpp:50`
   `KillMutexNative`). Primary path. handle64 был fallback. **Выбрано.**
2. Open-source клон handle64 — не надёжный supply chain.
3. Скачивание handle64.exe в runtime — лицензионно туманно.

**Решение Bundle H:** native fallback является primary, handle64.exe не
включается в bundle. Если на тестовой машине вылезет случай где native падает
(отказы DuplicateHandle на свежих Windows builds с tightened ACL?) — клиент
может вручную скачать handle64.exe и положить рядом с `DotaFarm.exe`. См.
README.txt → "Опциональные дополнения".

## HwidSpoofer.exe — статус

Файл `tools/spoofer/orchestrator/dist/HwidSpoofer.exe` отсутствует. Сборка
требует:
1. `cd tools/spoofer/orchestrator`
2. `pip install pyinstaller customtkinter` + других deps из `spoofer.py`
3. `pyinstaller --clean HwidSpoofer.spec --distpath dist`

По default'у `spooferEnabled=false` в `FarmConfig` — kernel-level спuфер не нужен
для большинства scenarios. Per-process HWID hooks через `ProxyHook.dll`
включены и self-contained. Считаем HwidSpoofer.exe optional add-on, не
блокирует Bundle H release. Если клиенту понадобится — собрать отдельно по
инструкции выше.

## ProxyHook.dll dumpbin BEFORE Bundle H

```
WS2_32.dll, ADVAPI32.dll, bcrypt.dll, IPHLPAPI.DLL, KERNEL32.dll,
MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll,
api-ms-win-crt-runtime-l1-1-0.dll, ... (8 api-ms-win-crt-*)
```

После Bundle H (через `add_subdirectory(... proxydivert/proxyrelay/proxyhook)`
с глобальным `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`) — VC runtime должны
уйти. Verify dumpbin после rebuild.

**Замечание:** ProxyHook.dll НЕ собирается из orchestrator/CMakeLists.txt — он
отдельный CMake project (`tools/dota2/proxyhook/CMakeLists.txt`). Static link
для него надо включать ОТДЕЛЬНО — добавил CMP0091/MSVC_RUNTIME setup в его
CMakeLists.

## PyInstaller bundle — спецификация

Файл: `tools/dota2/dota2_minify_wrapper_build/wrapper.spec`

**Build:**
```bash
cd tools/dota2
pip install pyinstaller vpk vdf
pyinstaller --clean dota2_minify_wrapper_build/wrapper.spec --distpath dist --workpath build_pyi
```

**Output:** `tools/dota2/dist/dota2_minify_wrapper.exe` (single file).

**Excludes:** tkinter, matplotlib, numpy, pandas, PIL, scipy, dearpygui,
lib2to3, test, unittest, pydoc_data — экономия ~10MB.

**hiddenimports:** `vpk`, `vdf` — wrapper их импортирует только если apply
вызывается с `--fix-launch-options`.

**UPX:** disabled. Reason: PyInstaller + UPX часто триггерит false positives у
Defender/Kaspersky/Avast (известный malware-pattern). Размер +5-8MB, но AV
clean.

**Vendor `dota2_minify/`** НЕ упаковывается внутрь .exe — `package.sh` копирует
его в `scripts/dota2_minify/` рядом с .exe. Wrapper читает `mods/blacklist.txt`
и `bin/blank-files/` через `os.path.dirname(__file__)` — в frozen mode
PyInstaller `__file__` указывает на путь .exe, поэтому `here/dota2_minify/`
найдётся. Это позволяет обновлять mods без rebuild .exe.

## Финальный bundle layout

```
DotaFarm/
  DotaFarm.exe                           ← static-linked, без VC++ Redist
  Andromeda-Dota2-Base.dll               ← already /MT
  ProxyHook.dll                          ← static-linked после Bundle H
  WinDivert.dll, WinDivert64.sys         ← legacy kernel proxy path
  sing-box.exe, wintun.dll               ← основной per-account SOCKS5
  README.txt                             ← инструкция для клиента
  config/
    accounts.json                        ← template (Steam credentials)
    farm.json                            ← template (relay creds, heroes, region)
  data/
    heroes.txt, ...                      ← если есть
  assets/
    fonts/, ...                          ← Cinzel + fallbacks для UI theme
  scripts/
    dota2_minify_wrapper.exe             ← PyInstaller bundle (zero-install)
    dota2_minify_wrapper.py              ← fallback для dev
    dota2_minify/                        ← Egezenn vendor (mods + blank-files)
      Minify/
        mods/<presetName>/blacklist.txt
        bin/blank-files/...
        LICENSE, README.md
    bots/                                ← Lua bot scripts (.dist_version marker)
  docs/                                  ← API_REFERENCE.md и др.
```

**Размер ожидаемый:** ~150-180 MB (главные занимающие — Egezenn vendor blank-files
~80MB + sing-box.exe ~30MB + DotaFarm.exe ~5MB + dota2_minify_wrapper.exe ~15MB +
Andromeda DLL ~2MB).

## Верификация после rebuild

```bash
# Должен показать ТОЛЬКО OS-shipped DLL — никаких MSVCP140/VCRUNTIME140/api-ms-win-crt-*
DUMPBIN="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/dumpbin.exe"
"$DUMPBIN" /DEPENDENTS build/Release/DotaFarm.exe | grep -i ".dll"

# Smoke test PyInstaller bundle на машине БЕЗ Python
where python  # пусть будет пусто
./dist/dota2_minify_wrapper.exe --help
./dist/dota2_minify_wrapper.exe status
```

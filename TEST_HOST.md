# DotaFarm — испытательный стенд

**Активный тест-хост: `217.60.245.131` (alias `dotahost` в `~/.ssh/config`)**

## Подключение

```bash
ssh dotahost
# то же что: ssh Administrator@217.60.245.131
# ключ: ~/.ssh/id_ed25519 (claude-on-aleks-pc)
```

Логин-only, без пароля. Раскатано через `tools/vds_deploy/install_host_ssh.bat`
(батник + публичный ключ собраны в `C:\temp\host_ssh_setup\`).

## Спецификация

| | |
|---|---|
| Hostname | VPS-455 |
| Платформа | QEMU/KVM VPS (Standard PC i440FX+PIIX 1996) |
| OS | Windows Server 2022 Standard Evaluation (build 20348) |
| CPU | AMD Ryzen 9 5950X — 4 vCPU выделено |
| RAM | 36 GB |
| Диск | C: 300 GB SSD, ~166 GB свободно |
| GPU | нет физической → Microsoft Basic Display + Remote Display Adapter → **D3D11 идёт через WARP** |
| FPS | юзер сказал «хоть 2 FPS, лишь бы функционал работал» — графика не важна |

## Что уже стоит

- Steam 2.10.91.91 (`C:\Program Files (x86)\Steam\steam.exe`)
- Dota 2 (через Steam)
- Sandboxie-Plus 1.12.9
- 7-Zip 26.01, VC++ 2010/2015-2022 redists, QEMU Guest Agent
- DotaFarm в `C:\Users\Administrator\AppData\Local\DotaFarm\`
  - `DotaFarm.exe` — текущий тест-билд (обновляется через `scp` напрямую при iteration на MM/inject)
  - `DotaFarm.log` — основной диагностический лог (тянуть через `scp dotahost:C:/Users/Administrator/AppData/Local/DotaFarm/DotaFarm.log /c/temp/DotaFarm.host.log`)
  - `debug_<PID>.log` — Andromeda DLL пишет per-PID после успешного inject (если их нет за сегодня — DllMain не отработал)
  - `dump_<PID>_<ts>.dmp` — crash dumps от watchdog'а

## Подводные камни

1. **Cloudbase-Init 1.1.6 активен** — он санитизирует `AutoAdminLogon` registry на каждом ребуте. Если будем делать unattended login для фермы — отключить service (по уроку со старого VDS [[vds-217-60-245-50]]).
2. **GPU=WARP** — Dota идёт через software D3D11, медленно. Для теста инжекта/MM/PEB ок, full match playthrough лагает.
3. **Auto-update DotaFarm НЕ работает в `%LOCALAPPDATA%\DotaFarm\`** — это не loader-managed dir, юзер положил EXE туда сам. При итерации обновляй EXE через `scp dotahost:C:\\Users\\Administrator\\AppData\\Local\\DotaFarm\\DotaFarm.exe` после `Stop-Process -Name DotaFarm,dota2 -Force`.

## Стандартный itter-loop

```bash
# 1. Правки в исходниках
# 2. Билд
cd "tools/dota2/orchestrator" && \
  "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build build --config Release --target DotaFarm

# 3. Package (vmp protect + sanity check)
bash package.sh

# 4. Push на хост напрямую (не через v1per.tech, чтобы не ждать download)
ssh dotahost "Stop-Process -Name DotaFarm,dota2 -Force -ErrorAction SilentlyContinue"
scp "/c/temp/DotaFarm/DotaFarm.exe" 'dotahost:C:\\Users\\Administrator\\AppData\\Local\\DotaFarm\\DotaFarm.exe'

# 5. Юзер запускает DotaFarm на хосте, скидывает лог
scp 'dotahost:C:/Users/Administrator/AppData/Local/DotaFarm/DotaFarm.log' /c/temp/DotaFarm.host.log

# 6. Параллельно при готовности — публичный deploy
cd "tools/dota2" && SKIP_REPACKAGE=1 bash deploy.sh
```

## Текущая исследовательская тема (2026-05-21)

**Andromeda inject через manual map крашит dota2.exe.** Breadcrumbs `prologue=1 rtlFT=1 cookie=1 tls=1 dllmain=0 epilogue=0` показали что DllMain VMProtect'нутой Andromeda сама вызывает `ExitProcess` (detects missing `PEB->Ldr` entry). Текущий fix (v `2026.05.21.2203`): для Andromeda **temp-file LoadLibrary первый**, MM secondary. ProxyHook остаётся MM-first (маленький, без VMP).

Если в будущем решим всё-таки делать MM для Andromeda — нужна полноценная **PEB.Ldr entry insertion** в shellcode (что делает BlackBone, есть в `C:\temp\andromeda_src\Andromeda-Injector\`).

# Themida 3.2.4 — как создать baseline .tmd templates

`protect.sh` ожидает 4 файла в этой папке:

| Файл | Тип | Аритектура | Применяется к |
|------|-----|------------|---------------|
| `template_x64_exe.tmd` | EXE | x64 | DotaFarm.exe, HwidSpoofer.exe |
| `template_x64_dll.tmd` | DLL | x64 | ProxyHook.dll, Andromeda-Dota2-Base.dll |
| `template_x86_exe.tmd` | EXE | x86 | (зарезервировано — пока не используется) |
| `template_x86_dll.tmd` | DLL | x86 | (зарезервировано — пока не используется) |

CLI Themida 3.2.4 принимает **только** `/protect <project.tmd> /inputfile=<src> /outputfile=<dst>` — настройки protection берутся **только** из `.tmd` файла, override через CLI нет. Поэтому baseline настройки нужно один раз задать через GUI.

## Procedure (один раз)

1. **Запустить GUI x64** для template_x64_exe.tmd:
   ```
   "C:\Users\aleks\Downloads\test4\Themida_x32_x64_v3.2.4\Themida64.exe"
   ```

2. **Application Information**:
   - Input File Name: указать любой временный x64 .exe (хотя бы build/Release/DotaFarm.exe — потом в CLI override через /inputfile)
   - Output File Name: temp путь, не важно

3. **Protection Options** (Mutation + Hotspots VM баланс согласно нашему плану):
   - **Virtualization** → ON, profile **Tiger Black** (баланс security ↔ speed) ИЛИ Mutation+Lion если хочется покрепче
   - **Mutation** → ON для Entry Point + RDTSC instructions
   - **Anti-Debugger Detection** → Advanced
   - **Anti-Dumper** → ON
   - **Memory Protection** → ON
   - **Resource Encryption** → ON
   - **VM-aware** options → balanced (avoid Ultra — Steam VAC может детектить тяжёлый VM footprint)
   - **Custom VM** → не нужно, hotspot VM-инструкции достаточно

4. **Macros** — опционально, можно оставить defaults. SDK markers (VM_START / MUTATE_START / etc.) пока в коде не используем (см. tools/dota2/protect/themida_sdk/ если решим добавить позже).

5. **Сохранить как** `tools/dota2/protect/templates/template_x64_exe.tmd`

6. **Повторить для DLL**:
   - File → New → Application Type = **DLL**
   - Те же настройки protection
   - Сохранить как `template_x64_dll.tmd`

7. **Smoke test**:
   ```bash
   bash tools/dota2/protect/protect.sh /tmp/test_protect
   ```

## Anti-VAC checklist (важно для ProxyHook.dll и Andromeda DLL)

- ❌ **НЕ включать** Tiger White / Dolphin (новые VM cores, легко сигнатурятся VAC)
- ❌ **НЕ включать** "Detect Mode Changes" (часто триггерит false positives в Steam процессах)
- ✓ **Включить** "Advanced API Wrapping" (скрывает GetSystemFirmwareTable / DeviceIoControl IAT хуки)
- ✓ **Включить** "Compress Resources" (уменьшает PE размер для AV эвристик)

## Why per-binary template?

Themida хранит `<application_type>` (EXE vs DLL) и связанные с этим параметры (entry point detection, TLS handling) внутри .tmd, и переопределить через CLI нельзя. Поэтому минимально нужны два template'а: для EXE и для DLL.

x86 templates созданы pre-emptively, но не нужны пока — все наши protected бинари сейчас x64 (Andromeda DLL — x64, ProxyHook.dll — x64).

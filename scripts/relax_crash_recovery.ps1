# Fix 4: Relax crash_recovery budget в farm.json
# Fix 5: Enable WerFault local dumps для dota2.exe + DotaFarm.exe
#
# Идемпотентно: повторный запуск создаёт новый backup и применяет одинаковые
# значения. Запускать на обоих стендах перед стартом DotaFarm после фикса
# рестарта.

$ErrorActionPreference = "Stop"

# ── Fix 4: farm.json crash_recovery relax ──
$f = "C:\Users\Administrator\AppData\Local\DotaFarm\config\farm.json"
$ts = Get-Date -Format "yyyy-MM-dd-HHmmss"
$bak = "$f.bak-$ts-crashfix"
Copy-Item $f $bak -Force
$j = Get-Content $f -Raw | ConvertFrom-Json

# Если crash_recovery нет вообще — создаём
if (-not $j.PSObject.Properties.Match("crash_recovery").Count) {
    $j | Add-Member -Type NoteProperty -Name crash_recovery -Value (New-Object PSObject)
}
$cr = $j.crash_recovery

function SetField($obj, $name, $value) {
    if ($obj.PSObject.Properties.Match($name).Count) {
        $obj.$name = $value
    } else {
        $obj | Add-Member -Type NoteProperty -Name $name -Value $value
    }
}

SetField $cr "max_crashes_per_window"          10    # было 3
SetField $cr "crash_window_min"                30    # было 5
SetField $cr "max_reconnects_per_match"         5    # было 3
SetField $cr "max_steam_relaunches_per_match"   5    # было 2
SetField $cr "loading_state_grace_s"          240    # было 90
SetField $cr "reconnect_cooldown_s"            15    # было 10
SetField $cr "enabled"                       $true
SetField $cr "steam_relaunch_enabled"        $true
SetField $cr "dump_watch_enabled"            $true
SetField $cr "respoof_hwid_on_relaunch"      $false  # держим как было

$j | ConvertTo-Json -Depth 10 | Set-Content $f -Encoding UTF8
Write-Host "[Fix4] farm.json crash_recovery relaxed. Backup: $bak"
$cr | Format-List

# ── Fix 5: WerFault LocalDumps ──
# Per-process dump policy: dota2.exe → C:\dumps\dota2\, type=2 (full).
# Источник: https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps
$wer = "HKLM:\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps"
if (-not (Test-Path $wer)) { New-Item -Path $wer -Force | Out-Null }

function SetupDumps($exeName, $folder) {
    $path = "$wer\$exeName"
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
    if (-not (Test-Path $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
    New-ItemProperty -Path $path -Name "DumpFolder"  -Value $folder -PropertyType ExpandString -Force | Out-Null
    New-ItemProperty -Path $path -Name "DumpCount"   -Value 10              -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $path -Name "DumpType"    -Value 2               -PropertyType DWord -Force | Out-Null  # 2 = Full dump
    New-ItemProperty -Path $path -Name "CustomDumpFlags" -Value 0           -PropertyType DWord -Force | Out-Null
    Write-Host "[Fix5] WerFault dumps configured: $exeName -> $folder"
}

SetupDumps "dota2.exe"    "C:\dumps\dota2"
SetupDumps "DotaFarm.exe" "C:\dumps\dotafarm"

# Enable WER service
$svc = Get-Service WerSvc -ErrorAction SilentlyContinue
if ($svc) {
    Set-Service -Name WerSvc -StartupType Automatic
    if ($svc.Status -ne "Running") { Start-Service WerSvc }
    Write-Host "[Fix5] WerSvc started/automatic"
}

Write-Host "ALL DONE"

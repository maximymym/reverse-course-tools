param(
    [string]$StandId = "stand_a",
    [string]$AuthToken = "1a9d0b80210bc74b8957c6ff13708051",
    [string]$Role = "master",
    [string]$PairSecret = "c3ec1dabbdc707ca9b693b123a0db78709b0e9f2d4f52a40bc11c2caab1cdc4b",
    [string]$RelayHost = "144.31.85.217"
)

$ErrorActionPreference = "Stop"
$f = "C:\Users\Administrator\AppData\Local\DotaFarm\config\farm.json"
if (-not (Test-Path $f)) { Write-Host "ERR: $f not found"; exit 1 }

$ts = Get-Date -Format "yyyy-MM-dd-HHmmss"
$bak = "$f.bak-$ts-pairing"
Copy-Item $f $bak -Force

$j = Get-Content $f -Raw | ConvertFrom-Json
$j.pairing.enabled = $true
$j.pairing.transport = "relay"
$j.pairing.role = $Role
$j.pairing.user_id = $StandId
$j.pairing.user_auth_token = $AuthToken
$j.pairing.pair_id = "two-stand-prod"
$j.pairing.pair_secret = $PairSecret
$j.pairing.relay_host = $RelayHost
$j.pairing.relay_port = 5050

$j | ConvertTo-Json -Depth 10 | Set-Content $f -Encoding UTF8

Write-Host "OK"
Write-Host "Backup: $bak"
Write-Host "Patched pairing:"
$j.pairing | Format-List

# display-ui branch checks (19 Oct 2026): boot the rig with Arnold arguments,
# then run drive.ps1 steps (shots, keys), then stop WinUAE. Copies Arnold.log.
# Usage: powershell -File winuae\display_ui_test.ps1 -Config Arnold-NTSC.uae -ArnoldArgs "MENU" -Tag ntsc-menu -Boot 30 -Steps "shot du-ntsc-menu"
param([string]$ArnoldArgs = "MENU", [string]$Tag = "du", [string]$Config = "Arnold-PiStorm.uae", [int]$Boot = 30, [string]$Steps = "")
$base = Split-Path -Parent $PSScriptRoot
$seq = "$base\winuae\wb\S\Startup-Sequence"
$latin1 = [Text.Encoding]::GetEncoding(28591)
$s = [IO.File]::ReadAllText($seq, $latin1)
$s = [regex]::Replace($s, '(?m)^Run >NIL: (Arnold:Arnold|Arnold:AmiCPC|Execute Arnold:Keys/batch).*$', ("Run >NIL: Arnold:Arnold " + $ArnoldArgs).TrimEnd())
[IO.File]::WriteAllText($seq, $s, $latin1)
Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
Start-Process "$base\winuae\bin\arnold-uae64.exe" -ArgumentList "-f `"$base\winuae\$Config`"" -WorkingDirectory "$base\winuae\bin"
Start-Sleep $Boot
& powershell -ExecutionPolicy Bypass -File "$base\winuae\drive.ps1" -Config $Config -Steps $Steps | Where-Object { $_ -notmatch '^focused' }
Start-Sleep 1
Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
Copy-Item "$base\run\Arnold.log" "$base\shots\display-ui\$Tag.log" -ErrorAction SilentlyContinue
# put the regression batch line back
$s = [IO.File]::ReadAllText($seq, $latin1)
$s = [regex]::Replace($s, '(?m)^Run >NIL: Arnold:Arnold.*$', "Run >NIL: Execute Arnold:Keys/batch")
[IO.File]::WriteAllText($seq, $s, $latin1)
Write-Output "done $Tag"

# Boot the rig with given Arnold arguments, capture at -Shots seconds, optionally
# press keypad fire (joystick fire 1) at -FireAt, then quit WinUAE.
# Usage: powershell -File winuae\boot_cart.ps1 -ArnoldArgs "Arnold:carts/PuzzleBobble.cpr" -Tag pb -Shots 35,50 -FireAt 40 [-Config Arnold-030.uae]
param([string]$ArnoldArgs = "", [string]$Tag = "run", [int[]]$Shots = @(35, 50), [int]$FireAt = 0, [string]$Config = "Arnold-PiStorm.uae")
$base = Split-Path -Parent $PSScriptRoot
$seq = "$base\winuae\wb\S\Startup-Sequence"
$latin1 = [Text.Encoding]::GetEncoding(28591)
$s = [IO.File]::ReadAllText($seq, $latin1)
$s = [regex]::Replace($s, '(?m)^Run >NIL: (Arnold:Arnold|Arnold:AmiCPC|Execute Arnold:Keys/batch).*$', ("Run >NIL: Arnold:Arnold " + $ArnoldArgs).TrimEnd())
[IO.File]::WriteAllText($seq, $s, $latin1)
Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
Start-Process "$base\winuae\bin\arnold-uae64.exe" -ArgumentList "-f `"$base\winuae\$Config`"" -WorkingDirectory "$base\winuae\bin"
$t0 = Get-Date
$events = @()
foreach ($t in $Shots) { $events += @{ t = $t; do = "shot $Tag-$t" } }
if ($FireAt -gt 0) { $events += @{ t = $FireAt; do = "key 52 60" } }
foreach ($e in ($events | Sort-Object { $_.t })) {
  while (((Get-Date) - $t0).TotalSeconds -lt $e.t) { Start-Sleep -Milliseconds 500 }
  & powershell -ExecutionPolicy Bypass -File "$base\winuae\drive.ps1" -Config $Config -Steps $e.do | Where-Object { $_ -notmatch '^focused' }
}
Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "done $Tag"

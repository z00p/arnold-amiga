# Build the Aminet LHA with the Amiga's own lha (winuae\wb\C\lha), as AmiMSX does:
# copies build\release\aminet\stage (Arnold drawer + Arnold.info) to run\Release
# (Arnold:Release), boots the test Workbench with the boot line replaced by the lha
# command, and waits for Arnold:Release/done. Output: build\release\aminet\Arnold.lha
# Follows the WinUAE lock protocol (C:\AMD\amimsx\build\winuae.lock).
param([string]$Config = "Arnold-PiStorm.uae", [int]$MaxWait = 240)
$ErrorActionPreference = "Stop"
$base = Split-Path -Parent $PSScriptRoot
$lock = "C:\AMD\amimsx\build\winuae.lock"
if (Get-Process arnold-uae64, winuae64 -ErrorAction SilentlyContinue) { throw "WinUAE is running" }
New-Item -ItemType Directory $lock | Out-Null   # fails (stops) if another session holds it
try {
  $src = Join-Path $base "build\release\aminet\stage"
  $rel = Join-Path $base "run\Release"
  if (Test-Path $rel) { Remove-Item -Recurse -Force $rel }
  New-Item -ItemType Directory $rel | Out-Null
  Copy-Item -Recurse (Join-Path $src "Arnold") $rel
  Copy-Item (Join-Path $src "Arnold.info") $rel
  $seq = "$base\winuae\wb\S\Startup-Sequence"
  $latin1 = [Text.Encoding]::GetEncoding(28591)
  $orig = [IO.File]::ReadAllText($seq, $latin1)
  $cmd = "CD Arnold:Release`nC:lha -r a Arnold.lha Arnold Arnold.info >Arnold:Release/lha.log`nC:lha v Arnold.lha >Arnold:Release/list.txt`nEcho >Arnold:Release/done ok"
  $s = [regex]::Replace($orig, '(?m)^Run >NIL: (Arnold:Arnold|Arnold:AmiCPC|Execute Arnold:Keys/batch).*$', $cmd.Replace('$', '$$'))
  [IO.File]::WriteAllText($seq, $s, $latin1)
  try {
    Start-Process "$base\winuae\bin\arnold-uae64.exe" -ArgumentList "-f `"$base\winuae\$Config`"" -WorkingDirectory "$base\winuae\bin"
    $t0 = Get-Date
    while (-not (Test-Path (Join-Path $rel "done")) -and ((Get-Date) - $t0).TotalSeconds -lt $MaxWait) { Start-Sleep 2 }
    Start-Sleep 2
  } finally {
    Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Stop-Process -Force
    [IO.File]::WriteAllText($seq, $orig, $latin1)
  }
  if (-not (Test-Path (Join-Path $rel "done"))) { throw "lha did not finish" }
  Copy-Item (Join-Path $rel "Arnold.lha") (Join-Path $base "build\release\aminet\Arnold.lha") -Force
  Get-Content (Join-Path $rel "list.txt")
} finally {
  Remove-Item $lock -ErrorAction SilentlyContinue
}

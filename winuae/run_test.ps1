# Boot the Arnold WinUAE rig, grab the WinUAE window after -Wait seconds, quit.
# Usage: powershell -File winuae\run_test.ps1 [-Config Arnold-PiStorm.uae] [-Wait 45] [-Shots 30,45]
param([string]$Config = "Arnold-PiStorm.uae", [int[]]$Shots = @(30, 45), [string]$Tag = "", [string]$ArnoldArgs = "")   # not $Args: that is PowerShell's own
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class TW {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  public struct RECT { public int L, T, R, B; }
  public static System.Drawing.Bitmap Grab(IntPtr h) {
    RECT r; GetWindowRect(h, out r);
    if (r.R - r.L < 10) return null;
    var bmp = new System.Drawing.Bitmap(r.R - r.L, r.B - r.T);
    using (var g = System.Drawing.Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
    return bmp;
  }
}
"@ -ReferencedAssemblies System.Drawing
[void][TW]::SetProcessDPIAware()
$base = Split-Path -Parent $PSScriptRoot
$winuae = "$base\winuae\bin\arnold-uae64.exe"   # renamed copy: invisible to the AmiMSX rig, which kills every winuae64
# The Startup-Sequence line that starts Arnold: "Run >NIL: Arnold:Arnold <args>"
$seq = "$base\winuae\wb\S\Startup-Sequence"
$latin1 = [Text.Encoding]::GetEncoding(28591)
$s = [IO.File]::ReadAllText($seq, $latin1)
$s = [regex]::Replace($s, '(?m)^Run >NIL: (Arnold:Arnold|Execute Arnold:Keys/batch).*$', ("Run >NIL: Arnold:Arnold " + $ArnoldArgs).TrimEnd())
[IO.File]::WriteAllText($seq, $s, $latin1)
Remove-Item "$base\run\boot.log" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$base\shots" | Out-Null
$proc = Start-Process $winuae -ArgumentList "-f `"$base\winuae\$Config`"" -WorkingDirectory (Split-Path $winuae) -PassThru
$t0 = Get-Date
foreach ($s in ($Shots | Sort-Object)) {
  while (((Get-Date) - $t0).TotalSeconds -lt $s) { Start-Sleep -Milliseconds 500 }
  $w = Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "*$Config*" } | Select-Object -First 1
  if (-not $w) { Write-Output "t=$s no window"; continue }
  $h = $w.MainWindowHandle
  $bmp = [TW]::Grab($h)
  if ($bmp) { $f = "$base\shots\$($Config -replace '\.uae$','')$Tag-$s.png"; $bmp.Save($f); Write-Output "t=$s saved $f ($($bmp.Width)x$($bmp.Height))" }
}
Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "*$Config*" } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 1
if (Test-Path "$base\run\boot.log") { Write-Output "--- boot.log"; Get-Content "$base\run\boot.log" | Select-Object -Last 6 }

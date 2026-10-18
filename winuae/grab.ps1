# Grab every WinUAE window into shots\<Tag>-<pid>.png (no start, no stop).
param([string]$Tag = "grab", [switch]$Stop)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class TW2 {
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
[void][TW2]::SetProcessDPIAware()
$base = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force "$base\shots" | Out-Null
foreach ($p in (Get-Process arnold-uae64 -ErrorAction SilentlyContinue)) {
  if ($p.MainWindowHandle -eq 0) { Write-Output "pid $($p.Id): no window"; continue }
  $bmp = [TW2]::Grab($p.MainWindowHandle)
  if ($bmp) { $f = "$base\shots\$Tag-$($p.Id).png"; $bmp.Save($f); Write-Output "pid $($p.Id): $f ($($bmp.Width)x$($bmp.Height)) title '$($p.MainWindowTitle)'" }
}
if ($Stop) { Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Stop-Process -Force; Write-Output "stopped" }

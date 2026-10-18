# Boot the rig, press keyboard-joystick fire (numpad 0, WinUAE "kbd1" layout) at
# -Fire seconds, grab, then End (= Amiga Help: Arnold quits) at -Quit seconds, grab, stop.
# Only ever touches the WinUAE window whose title names our config (the AmiMSX
# regression rig may be running its own WinUAE sessions at the same time).
param([string]$Config = "Arnold-PiStorm.uae", [int]$Fire = 30, [int]$Quit = 42)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class TW3 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
  public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] public struct IU { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public long pad1; [FieldOffset(8)] public long pad2; [FieldOffset(16)] public long pad3; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public IU u; }
  // Press and release a key by hardware scan code (0x52 = numpad 0, 0x4F = End as extended).
  public static void Scan(ushort scan, bool ext, int holdMs) {
    uint f = 0x0008 | (ext ? 0x0001u : 0u);   // KEYEVENTF_SCANCODE [| EXTENDEDKEY]
    var d = new INPUT[1]; d[0].type = 1; d[0].u.ki.wScan = scan; d[0].u.ki.dwFlags = f;
    SendInput(1, d, Marshal.SizeOf(typeof(INPUT)));
    System.Threading.Thread.Sleep(holdMs);
    d[0].u.ki.dwFlags = f | 0x0002;             // KEYEVENTF_KEYUP
    SendInput(1, d, Marshal.SizeOf(typeof(INPUT)));
  }
  public static System.Drawing.Bitmap Grab(IntPtr h) {
    RECT r; GetWindowRect(h, out r);
    if (r.R - r.L < 10) return null;
    var bmp = new System.Drawing.Bitmap(r.R - r.L, r.B - r.T);
    using (var g = System.Drawing.Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
    return bmp;
  }
}
"@ -ReferencedAssemblies System.Drawing
[void][TW3]::SetProcessDPIAware()
$base = Split-Path -Parent $PSScriptRoot
$winuae = "$base\winuae\bin\arnold-uae64.exe"   # renamed copy: invisible to the AmiMSX rig, which kills every winuae64
$tag = $Config -replace '\.uae$', ''
Write-Output ("joyports: " + ((Get-Content "$base\winuae\$Config") -match '^joyport' -join ' '))
$proc = Start-Process $winuae -ArgumentList "-f `"$base\winuae\$Config`"" -WorkingDirectory (Split-Path $winuae) -PassThru
$t0 = Get-Date
Start-Sleep 5; $proc.Refresh(); Write-Output "launcher pid $($proc.Id) exited=$($proc.HasExited) code=$($proc.ExitCode)"
Get-Process arnold-uae64 -ErrorAction SilentlyContinue | ForEach-Object { Write-Output "  winuae $($_.Id) '$($_.MainWindowTitle)'" }
function Ours { Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "*$Config*" } | Select-Object -First 1 }
function Shot($s) { $p = Ours; if ($p) { $b = [TW3]::Grab($p.MainWindowHandle); $f = "$base\shots\$tag-input-$s.png"; $b.Save($f); Write-Output "saved $f" } else { Write-Output "no Arnold window for $s" } }
function Key($scan, $ext) { $p = Ours; if ($p) { [void][TW3]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 400; [TW3]::Scan($scan, $ext, 120); Write-Output "sent scan $scan" } }
while (((Get-Date) - $t0).TotalSeconds -lt $Fire) { Start-Sleep -Milliseconds 500 }
Shot "0-before"
Key 0x52 $false; Start-Sleep 3; Shot "1-fire"
Key 0x52 $false; Start-Sleep 3; Shot "2-fire2"
while (((Get-Date) - $t0).TotalSeconds -lt $Quit) { Start-Sleep -Milliseconds 500 }
Key 0x4F $true; Start-Sleep 4; Shot "3-help"
$p = Ours; if ($p) { Stop-Process -Id $p.Id -Force; Write-Output "stopped $($p.Id)" }
Write-Output "done"

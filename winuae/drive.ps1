# Drive the running Arnold WinUAE window: a list of steps in one string.
#   click X Y     left click at window-relative pixel (as in the grab.ps1 captures)
#   down/up SCAN VK [e]  hold / release a key (several keys at once)
#   key SCAN VK [e]  press a key: PC scan code and virtual key (hex), 'e' = extended (End = 4F 23 e = Amiga Help)
#   wait N        seconds
#   shot TAG      capture to shots\TAG.png
# Usage: powershell -File winuae\drive.ps1 "click 225 1050; wait 5; shot game; key 4F e; wait 3; shot menu"
param([string]$Steps = "", [string]$Config = "Arnold-PiStorm.uae")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class TW4 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  // Windows only lets the foreground process hand over focus: pressing Alt first is the usual key.
  public static bool Focus(IntPtr h) {
    for (int i = 0; i < 5 && GetForegroundWindow() != h; i++) {
      keybd_event(0x12, 0x38, 0, UIntPtr.Zero); keybd_event(0x12, 0x38, 2, UIntPtr.Zero);
      SetForegroundWindow(h); System.Threading.Thread.Sleep(250);
    }
    return GetForegroundWindow() == h;
  }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
  public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] public struct IU { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public long p1; [FieldOffset(8)] public long p2; [FieldOffset(16)] public long p3; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public IU u; }
  public static void Scan(ushort scan, bool ext, int holdMs) {
    uint f = 0x0008 | (ext ? 0x0001u : 0u);
    var d = new INPUT[1]; d[0].type = 1; d[0].u.ki.wScan = scan; d[0].u.ki.dwFlags = f;
    SendInput(1, d, Marshal.SizeOf(typeof(INPUT)));
    System.Threading.Thread.Sleep(holdMs);
    d[0].u.ki.dwFlags = f | 0x0002;
    SendInput(1, d, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Click(int x, int y) {
    // WinUAE's magic mouse syncs the Amiga pointer on mouse-move messages:
    // approach in steps and let it settle before pressing.
    SetCursorPos(x - 40, y - 20); System.Threading.Thread.Sleep(120);
    SetCursorPos(x - 8, y - 4); System.Threading.Thread.Sleep(120);
    SetCursorPos(x, y); System.Threading.Thread.Sleep(120);
    SetCursorPos(x + 1, y); System.Threading.Thread.Sleep(120);
    SetCursorPos(x, y); System.Threading.Thread.Sleep(250);
    mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(200);
    mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(200);
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
[void][TW4]::SetProcessDPIAware()
$base = Split-Path -Parent $PSScriptRoot
function Ours { Get-Process arnold-uae64 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "*$Config*" } | Select-Object -First 1 }
$p = Ours
if (-not $p) { Write-Output "no Arnold WinUAE window"; exit 2 }
$h = $p.MainWindowHandle
if (-not [TW4]::Focus($h)) { Write-Output "could not bring WinUAE to the foreground" } else { Write-Output "focused" }
foreach ($step in ($Steps -split ';')) {
  $t = $step.Trim() -split '\s+'
  if ($t.Count -eq 0 -or $t[0] -eq '') { continue }
  switch ($t[0]) {
    'click' { $r = New-Object TW4+RECT; [void][TW4]::GetWindowRect($h, [ref]$r); [TW4]::Click($r.L + [int]$t[1], $r.T + [int]$t[2]); Write-Output "click $($t[1]) $($t[2])" }
    'key'   { # key SCAN VK [e]: keybd_event with scan code and virtual key, as the AmiMSX harness does
              if (-not [TW4]::Focus($h)) { Write-Output "not foreground; key skipped"; continue }
              $sc = [byte][Convert]::ToUInt16($t[1], 16); $vk = [byte][Convert]::ToUInt16($t[2], 16)
              $fl = [uint32]0x8; if ($t.Count -gt 3 -and $t[3] -eq 'e') { $fl = [uint32]0x9 }
              [TW4]::keybd_event($vk, $sc, $fl, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120
              [TW4]::keybd_event($vk, $sc, ($fl -bor [uint32]2), [UIntPtr]::Zero); Write-Output "key $($t[1])/$($t[2])" }
    'down'  { # down SCAN VK [e]: press and hold (for several keys at once; release with 'up')
              if (-not [TW4]::Focus($h)) { Write-Output "not foreground; key skipped"; continue }
              $sc = [byte][Convert]::ToUInt16($t[1], 16); $vk = [byte][Convert]::ToUInt16($t[2], 16)
              $fl = [uint32]0x8; if ($t.Count -gt 3 -and $t[3] -eq 'e') { $fl = [uint32]0x9 }
              [TW4]::keybd_event($vk, $sc, $fl, [UIntPtr]::Zero); Write-Output "down $($t[1])" }
    'up'    { $sc = [byte][Convert]::ToUInt16($t[1], 16); $vk = [byte][Convert]::ToUInt16($t[2], 16)
              $fl = [uint32]0xA; if ($t.Count -gt 3 -and $t[3] -eq 'e') { $fl = [uint32]0xB }
              [TW4]::keybd_event($vk, $sc, $fl, [UIntPtr]::Zero); Write-Output "up $($t[1])" }
    'wait'  { Start-Sleep -Milliseconds ([int]([double]$t[1] * 1000)) }
    'shot'  { $b = [TW4]::Grab($h); if ($b) { $f = "$base\shots\$($t[1]).png"; $b.Save($f); Write-Output "shot $f" } }
    default { Write-Output "unknown step: $step" }
  }
}

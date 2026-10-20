# Synthesizes real mouse/keyboard input for desktop smoke tests (moves the real cursor).
# Usage: input.ps1 "move 500 400" "down" "move 600 420" "up" "wheel -120" "keys ctrl+shift+d" "sleep 200"
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Inp {
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] public struct U { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public U u; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  public static void Mouse(uint flags, int data) {
    var i = new INPUT[1]; i[0].type = 0; i[0].u.mi.dwFlags = flags; i[0].u.mi.mouseData = (uint)data;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Key(ushort vk, bool up) {
    var i = new INPUT[1]; i[0].type = 1; i[0].u.ki.wVk = vk; i[0].u.ki.dwFlags = up ? 2u : 0u;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
}
"@
[Inp]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$vk = @{ ctrl = 0x11; shift = 0x10; alt = 0x12; esc = 0x1B; capslock = 0x14; del = 0x2E; up = 0x26; down = 0x28; minus = 0xBD; plus = 0xBB }
foreach ($step in $Steps) {
  $p = $step -split ' '
  switch ($p[0]) {
    'move'  { [Inp]::SetCursorPos([int]$p[1], [int]$p[2]) | Out-Null; [Inp]::Mouse(0x0001, 0) }
    'down'  { [Inp]::Mouse(0x0002, 0) }
    'up'    { [Inp]::Mouse(0x0004, 0) }
    'wheel' { [Inp]::Mouse(0x0800, [int]$p[1]) }
    'sleep' { Start-Sleep -Milliseconds ([int]$p[1]) }
    'drag'  { # drag x1 y1 x2 y2 steps
      $x1=[int]$p[1]; $y1=[int]$p[2]; $x2=[int]$p[3]; $y2=[int]$p[4]; $n=[int]$p[5]
      [Inp]::SetCursorPos($x1, $y1) | Out-Null; [Inp]::Mouse(0x0001, 0); [Inp]::Mouse(0x0002, 0)
      for ($k = 1; $k -le $n; $k++) { [Inp]::SetCursorPos([int]($x1 + ($x2-$x1)*$k/$n), [int]($y1 + ($y2-$y1)*$k/$n)) | Out-Null; [Inp]::Mouse(0x0001, 0); Start-Sleep -Milliseconds 8 }
      [Inp]::Mouse(0x0004, 0)
    }
    'keys'  { # keys ctrl+shift+d
      $codes = $p[1] -split '\+' | ForEach-Object { if ($vk.ContainsKey($_)) { $vk[$_] } else { [int][char]$_.ToUpper() } }
      foreach ($c in $codes) { [Inp]::Key([uint16]$c, $false) }
      [array]::Reverse($codes); foreach ($c in $codes) { [Inp]::Key([uint16]$c, $true) }
    }
  }
}

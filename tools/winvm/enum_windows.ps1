# Runs INSIDE the guest's interactive console session (see systest.py): a
# WinRM process lives in a different session and cannot see the game's windows.
# Writes one block per echovr.exe: its top-level windows and their child text,
# which is where a blocking MessageBox's message lives.
param([Parameter(Mandatory)][string]$OutFile)

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static List<string> Dump(uint pid) {
    var o = new List<string>();
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) {
        var t = new StringBuilder(512); var c = new StringBuilder(256);
        GetWindowText(h, t, 512); GetClassName(h, c, 256);
        o.Add("TOP hwnd=" + h + " class=" + c + " visible=" + IsWindowVisible(h) + " title=[" + t + "]");
        EnumChildWindows(h, (ch, l2) => {
          var t2 = new StringBuilder(2048); var c2 = new StringBuilder(256);
          GetWindowText(ch, t2, 2048); GetClassName(ch, c2, 256);
          o.Add("   child class=" + c2 + " text=[" + t2 + "]"); return true; }, IntPtr.Zero);
      }
      return true; }, IntPtr.Zero);
    return o;
  }
}
"@

$pids = @(Get-Process echovr -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
$out = @("echovr pids: $($pids -join ',')  session: $((Get-Process -Id $PID).SessionId)")
foreach ($p in $pids) { $out += "== pid $p =="; $out += [W]::Dump([uint32]$p) }
$out | Set-Content -Path $OutFile -Encoding UTF8

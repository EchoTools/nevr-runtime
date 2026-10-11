nEVR runtime @VERSION@  (commit @COMMIT@)

What this is
  The nEVR runtime for Echo VR on Windows: one file, BugSplat64.dll, that replaces the game's
  BugSplat64.dll. It needs no config file of any kind: the service endpoints and the public keys are
  built in.

Install
  1. Close Echo VR.
  2. Unzip this package anywhere.
  3. Run, in a PowerShell window opened in the unzipped folder:
       powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
     If you have more than one Echo VR install, or the script cannot find yours, add
       -Dir "C:\...\ready-at-dawn-echo-arena\bin\win10"
  The script checks the package against SHA256SUMS, copies your original BugSplat64.dll to
  BugSplat64.dll.original-<timestamp> next to it, renames a legacy dbgcore.dll to
  dbgcore.dll.legacy-<date> (the runtime refuses to start beside one), and installs the new file.
  It deletes nothing.

Uninstall
  Close Echo VR, then:
       powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 -Dir "<the same bin\win10 folder>"
  This puts your original BugSplat64.dll and a legacy dbgcore.dll back. The backup files stay.

Signing
  SIGNING.txt in this package says how to check whether each file is signed. A file that is not signed may be
  flagged by Windows Defender or SmartScreen. The SHA-256 of BugSplat64.dll is in SHA256SUMS; compare it with
  the value you were given.

Logs
  The runtime writes its log under %LOCALAPPDATA%\EchoVR\logs. When you report a problem, send the
  newest nevr-*.jsonl file from that folder.

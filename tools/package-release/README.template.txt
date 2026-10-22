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
     If the script cannot find your install it asks for the folder that holds echovr.exe
     (...\ready-at-dawn-echo-arena\bin\win10); type or paste it. If you have more than one Echo VR
     install, add
       -Dir "C:\...\ready-at-dawn-echo-arena\bin\win10"
  The script checks the package against SHA256SUMS, copies your original BugSplat64.dll to
  BugSplat64.dll.original-<timestamp> next to it, renames a legacy dbgcore.dll to
  dbgcore.dll.legacy-<date> (the runtime refuses to start beside one), and installs the new file.
  It deletes nothing.

Update
  Unzip the newer package and run its install.ps1 the same way. When the BugSplat64.dll in the folder is
  one this folder's install put there, the script replaces it with the new one and keeps your original
  BugSplat64.dll backup as it was; uninstall.ps1 then still puts your original back. It stops and leaves
  the file alone when it is one the script did not put there.

Uninstall
  Close Echo VR, then:
       powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 -Dir "<the same bin\win10 folder>"
  This puts your original BugSplat64.dll and a legacy dbgcore.dll back. The backup files stay.

If the Quest build will not start
  Connect the headset by USB (USB debugging on), then:
       powershell -NoProfile -ExecutionPolicy Bypass -File quest-diagnose.ps1
  It reads the headset's facts, starts the game once, shows a short verdict and writes
  nevr-diagnose-YYYYMMDDTHHMMSSZ.zip next to the script. It installs and deletes nothing on the headset. On the
  headset it closes the game if it is running, erases the headset's log buffer (after saving a copy) and starts
  the game once. It does not copy your sign-in link. It replaces the headset's serial numbers and any e-mail
  address with <device-serial> and <email>, and masks nothing else: the zip holds the runtime's crash dumps as
  they are and the headset's whole system log, which can show your Wi-Fi name, its BSSID, IP addresses and numeric user ids.
  Send the zip by direct message to sprockee on the EchoVRCE Discord, never in a public channel.

Signing
  SIGNING.txt in this package says how to check whether each file is signed. A file that is not signed may be
  flagged by Windows Defender or SmartScreen. The SHA-256 of BugSplat64.dll is in SHA256SUMS; compare it with
  the value you were given.

Logs
  The runtime writes its log under %LOCALAPPDATA%\EchoVR\logs. When you report a problem, send the
  newest nevr-*.jsonl file from that folder.

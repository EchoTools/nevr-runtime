nEVR runtime @VERSION@  (commit @COMMIT@)

What this is
  The nEVR runtime for Echo VR on Windows: one file, BugSplat64.dll, that replaces the game's
  BugSplat64.dll. A player's game needs no config file of any kind: the service endpoints and the public keys
  are built in. A dedicated server does need one (see Running a dedicated server), and echovr_server.exe,
  the server's launcher, is in this package too.

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

Running a dedicated server
  echovr_server.exe in this package starts a dedicated server. install.ps1 does not copy it. Install
  BugSplat64.dll as above, then copy echovr_server.exe into the same folder as echovr.exe
  (...\ready-at-dawn-echo-arena\bin\win10) and run it from a console there:
       .\echovr_server.exe
  It starts echovr.exe -server -headless -no-console from its own folder and passes any arguments on. A server
  has no built-in service settings yet (issue #843), so it needs a config file that sets services.socket_uri,
  and without one it exits.
  Create ...\ready-at-dawn-echo-arena\_local\config.yaml. The smallest one that works:

       services:
         socket_uri: "wss://<the game service's websocket address>"
       auth:
         http_uri: "https://<the game service's http address>"
         http_key: "<the service's public API key>"
         password: "<the password of your server account>"
       identity:
         discord_id: "<the Discord id of your server account>"

  The two addresses and the public API key are the ones every client has built in; ask the service operator
  for them. A server does not read auth.server_key. The password goes in auth.password, never in the
  socket_uri. A server signs in with the account's refresh token when it has one, and with auth.password
  otherwise. The server logs in, registers with the game service and logs "registration successful"; every
  step is in the log (see Logs). Stop it with Ctrl+C in its console: the log then says "Graceful shutdown
  initiated". services.allow_offline_server: true starts a server with no service at all (it never
  registers); docs/reference/example-config.yaml in the source repository lists every key.

Update
  Unzip the newer package and run its install.ps1 the same way. When the BugSplat64.dll in the folder is
  one this folder's install put there, the script replaces it with the new one and keeps your original
  BugSplat64.dll backup as it was; uninstall.ps1 then still puts your original back. It stops and leaves
  the file alone when it is one the script did not put there. If the BugSplat64.dll in the folder is already
  this package's (copied in by hand), install.ps1 records it so uninstall.ps1 can undo it. uninstall.ps1 also
  accepts any NEVR runtime DLL (one carrying the NEVR-BUILD identity text) in the folder.

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

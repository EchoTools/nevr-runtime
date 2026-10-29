nEVR runtime @VERSION@  (commit @COMMIT@)

What this is
  The nEVR runtime for Echo VR on Windows: two files that go in together. BugSplat64.dll goes in the game's
  folder, replaces the game's BugSplat64.dll and is the small host the game loads. nevr-runtime.dll holds the
  runtime and goes in your nEVR folder, %LOCALAPPDATA%\nevr\echovr; the host checks its signature and loads it
  from there, and stops the game with a message when it is missing or not signed by the nEVR release process.
  A player's game needs no config file of any kind: the service endpoints and the public keys
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
  The script checks nevr-install.exe against SHA256SUMS and runs it. nevr-install.exe is the core installer: it
  fetches the signed release manifest and checks its signature, and installs the two DLLs in this package when
  their SHA-256 matches the manifest's entry for the release; when they are missing or differ it downloads the
  package the manifest names and checks that instead. It copies your original BugSplat64.dll to
  BugSplat64.dll.original-<timestamp> in your nEVR folder (installer\aside), moves a legacy dbgcore.dll to
  dbgcore.dll.legacy-<date> (the runtime refuses to start beside one), and installs the two new files. An
  nevr-runtime.dll already in your nEVR folder is renamed to nevr-runtime.dll.original-<timestamp> there. An
  nEVR runtime an earlier release left beside echovr.exe (the host no longer looks there) is renamed to
  nevr-runtime.dll.legacy-<timestamp>. It deletes nothing, writes only BugSplat64.dll in the game folder, puts
  every file back as it was if a step fails, and writes a log (a [NEVR.INSTALL] JSON file) whose path it
  prints. When the signed manifest cannot be fetched, the core uses the copy it stored on an earlier run if that
  copy is still valid; with none stored it stops before it writes anything. To use another nEVR folder, add -Root "<folder>".

Running a dedicated server
  echovr_server.exe in this package starts a dedicated server. install.ps1 does not copy it. Install
  both DLLs as above, then copy echovr_server.exe into the same folder as echovr.exe
  (...\ready-at-dawn-echo-arena\bin\win10) and run it from a console there:
       .\echovr_server.exe
  It starts echovr.exe -server -headless -no-console from its own folder and passes any arguments on. A server
  takes the game service's addresses and public API key from the build, as a player's game does (issue #843),
  so its config file needs only its own account.
  Create ...\ready-at-dawn-echo-arena\_local\config.yaml (KNOWN-WRONG #1255: the runtime does not read it from
  the nevr folder). The smallest one that works:

       auth:
         password: "<the password of your server account>"
       identity:
         discord_id: "<the Discord id of your server account>"

  The password goes in auth.password. A server does not read auth.server_key. A server signs in with the
  account's refresh token when it has one, and with auth.password otherwise. To run it against another service,
  set services.socket_uri, auth.http_uri and auth.http_key in the same file too. The server logs in, registers
  with the game service, and logs "requested game server registration" and then "Received registration success
  via protobuf"; every step is in the log (see Logs). Stop it with Ctrl+C in its console: the log then says "Graceful shutdown
  initiated". services.socket_uri: "" with services.allow_offline_server: true starts a server with no service at all (it never
  registers); docs/reference/example-config.yaml in the source repository lists every key.

Update
  Unzip the newer package and run its install.ps1 the same way. When the BugSplat64.dll in the folder is
  one this folder's install put there, the installer replaces it and the nevr-runtime.dll in your nEVR folder with the new ones and keeps
  your original BugSplat64.dll backup as it was; uninstall.ps1 then still puts your original back. If
  the update fails, both files are put back as they were. It stops and leaves
  the file alone when it is one the script did not put there. If the BugSplat64.dll in the folder is already
  this package's (copied in by hand), install.ps1 records it so uninstall.ps1 can undo it. uninstall.ps1 also
  accepts any NEVR runtime DLL (one carrying the NEVR-BUILD identity text, or the older nevr-runtime/1.0 user agent) in the folder.

Uninstall
  Close Echo VR, then:
       powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 -Dir "<the same bin\win10 folder>"
  (add -Root "<folder>" if you gave install.ps1 one). This puts your original BugSplat64.dll and a legacy
  dbgcore.dll back and sets nevr-runtime.dll aside, in your nEVR folder (installer\aside), as
  nevr-runtime.dll.uninstalled-<timestamp>. A runtime install.ps1 moved out of the game folder goes back there.
  The backup files stay.

If the Quest build will not start
  Connect the headset by USB (USB debugging on), then:
       powershell -NoProfile -ExecutionPolicy Bypass -File quest-diagnose.ps1
  It reads the headset's facts, starts the game once, shows a short verdict and writes
  nevr-diagnose-YYYYMMDDTHHMMSSZ.zip next to the script. It installs and deletes nothing on the headset. On the
  headset it closes the game if it is running, erases the headset's log buffer (after saving a copy) and starts
  the game once. It does not copy your sign-in link. It replaces the headset's serial numbers and any e-mail
  address with <device-serial> and <email>, and masks nothing else: the zip holds the runtime's crash dumps as
  they are and the headset's whole system log, which can show your Wi-Fi name, its BSSID, IP addresses and numeric user ids.
  Describe the problem on the Beta bug report form, https://github.com/EchoTools/nevr-runtime/issues/new?template=beta-bug.yml.
  Do not attach it to a public GitHub issue; send it to a maintainer by direct message on the EchoVRCE Discord. It holds
  your Wi-Fi name, IP addresses and numeric user ids, and it is never in a public channel.

Signing
  SIGNING.txt in this package says how to check whether each file is signed. A file that is not signed may be
  flagged by Windows Defender or SmartScreen. The SHA-256 of both DLLs is in SHA256SUMS; compare it with
  the value you were given.

Logs
  The runtime writes its log under %LOCALAPPDATA%\nevr\echovr\logs. When you report a problem, send the
  newest nevr-*.jsonl file from that folder.

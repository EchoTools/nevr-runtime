# nEVR Runtime

nEVR Runtime keeps Echo VR (`echovr.exe`) playable on the community game service
([echovrce](https://github.com/echotools), Nakama-based). Drop two files,
`BugSplat64.dll` into the game folder and `nevr-runtime.dll` into your nEVR folder (`%LOCALAPPDATA%\nevr\echovr`) and the game signs in with your Discord
account, reaches the social lobby and shows its friends and party. The Quest
build does the same through a repacked APK. For contributors it is the C++17
source of those DLLs and of the Quest sentinel, plus the tooling and tests that
keep both honest.

Part of the **NEVR** project, keeping Echo VR alive.

> Working in this repo as an agent? Start at [`AGENTS.md`](AGENTS.md).

## Built on, and thanks

nEVR is a community project, built on other people's work and tested by the people who reported what broke. These are the projects and people it builds on, and what each did.

<!-- credits:begin (generated from config/credits.json by tools/render_credits.py; edit the JSON, then run it with --write) -->

### Projects

- **[Xenomega](https://github.com/Xenomega)**: [EchoRelay](https://github.com/Xenomega/EchoRelay), a reimplementation of Echo VR's web services and dedicated game servers. The legacy message translation in `docs/reference/legacy-translator.md` is read from its documentation.
- **kfc**: Helped write a lot of EchoRelay.
- **[Dualgame](https://github.com/Dualgame)**: [echo-symbols](https://github.com/Dualgame/echo-symbols), the recovered symbol names this repo pulls at a pinned commit for its log symbol cache, and [echovr-modkit](https://github.com/Dualgame/echovr-modkit) with its [EchoRelay fork](https://github.com/Dualgame/EchoRelay), which back invite codes and the early-quit lockout and which `docs/engine/early-quit.md` and the invite-code design (#906) draw on.
- **[marshmallow-mia](https://github.com/marshmallow-mia)**: The Echo VR Linux port, the [Echo VR installer](https://github.com/marshmallow-mia/Echo-VR-Installer) (PC and Quest) and [EchoVR_Launcher](https://github.com/marshmallow-mia/EchoVR_Launcher) (Windows and Linux). Reported #133 to #137, #495, #496, #511 to #513 and #719, and wrote the merged fixes #138 to #141, #500, #502, #514 to #516 and #721. [NvrContentOverlay](https://github.com/marshmallow-mia/NvrContentOverlay), the plugin that serves a content pack's game data in place of the stock files. Tested nevr-runtime in #nevr-runtime-testing.
- **[Crafter](https://github.com/Crafter-1)**: [Quest-EchoVR-Installer](https://github.com/Crafter-1/Quest-EchoVR-Installer), a 3D Quest installer for Echo VR built in Unity. Reported #621, the game sending a Quest 3 player back to Home at launch, with a full logcat. Tested nevr-runtime in #nevr-runtime-testing.
- **Ducky**: The reverse engineer behind the Echo Combat Quest port.
- **[babs](https://github.com/nmdurkee)**: Echo Frenzy, a community game mode for Echo VR that paved the way for others making game modes; the pipeline for editing Echo's in-game screens (Doom on Echo is built on it); EchoCraft; and the moderator tablet for kicking players from lobbies. Has been there every step of the way, before and since Echo's shutdown.
- **[EchoCombat](https://github.com/heisthecat31/EchoCombat)**: he_is_the_cat's new and fixed Echo Combat gear (DOT FIELD, DRONE, UMBRAL VEIL, OVERCHARGE, NEUTRON, TRANSLOCATOR) as nevr-runtime plugins and a content pack.

### People

- **Trodd-**: Found the XInput fix that lets the spectator free cam see Xbox One and Series controllers, and brought it to the project (#877). Built the echovrce.com website. Tested nevr-runtime in #nevr-runtime-testing.
- **DinkyBoi**: Reported a party of three queuing for arena with one member left out (#888), and the arm tablet's repeated "server full" message when joining a full social lobby (#889). Tested nevr-runtime in #nevr-runtime-testing.
- **berg**: One of the party of three in #888: left out of the arena match once, and rejoined through a link. Tested nevr-runtime in #nevr-runtime-testing.
- **zebra**: One of the party of three in #888, left out of the arena match. Tested nevr-runtime in #nevr-runtime-testing.
- **Nebula**: Reported install.ps1 failing on a PC with no D: drive (#679) and the idea that it asks for the game folder (#680), the Quest game closing at start (#758), uninstall.ps1 refusing after an update (#759) and the tablet's missing Add friend button (#775). Tested nevr-runtime in #nevr-runtime-testing.
- **CyanoTex**: Tested nevr-runtime in #nevr-runtime-testing.
- **spike4887**: Tested nevr-runtime in #nevr-runtime-testing.
- **TekEmperor**: Tested nevr-runtime in #nevr-runtime-testing.
- **arii**: Tested nevr-runtime in #nevr-runtime-testing.
- **[DC Coffey](https://www.dccoffey.com/work/echo-vr)**: Designed the invite and create-game menu (CREATE GAME and JOIN VIA CODE), which Dualgame implemented.
- **[chinmayee5599](https://github.com/chinmayee5599)**: Opened #510, which renamed three TCP game message symbols in `src/abi/symbols.h`.
- **[brysonak](https://github.com/brysonak)**: Opened #12, which changed the server log file names and added a flag to keep the old names.
- **Furo**: Supporter of nevr-runtime.

### Thanks

- **AvaGoosa**: Named in the thank-you strings of the Echo VR installer's patcher.
- **h4ck3r210**: Named in the thank-you strings of the Echo VR installer's patcher.
- **Elominator**: Named in the thank-you strings of the Echo VR installer's patcher.
- **sprockee**: Named in the thank-you strings of the Echo VR installer's patcher.
- **he_is_the_cat**: Named in the thank-you strings of the Echo VR installer's patcher. Tested nevr-runtime in #nevr-runtime-testing.

Thank you to everyone in #nevr-runtime-testing, named or not.

<!-- credits:end -->

## What it does

PC means the Windows or Wine game client, Quest means the Android sentinel, both
means each. Every feature below is on `main`; [`docs/testing/smoke-checklist.md`](docs/testing/smoke-checklist.md)
says how to check each one and which log line proves it.

### Get in
- **Nothing to configure.** The service endpoints and the public client keys are built in, so a game client with a build-embedded configuration signs in with no `config.yaml`. A `config.yaml` overrides them, on the PC next to the game and on the Quest in the app's files folder ([`docs/reference/example-config.yaml`](docs/reference/example-config.yaml) lists every key and which builds read it). (both)
- **Sign in with Discord.** A device-code sign-in in your browser; the saved sign-in is reused on the next start and refreshed in the background. (PC)
- **Saved sign-in on the headset.** The Quest build reuses its saved sign-in, and shows a sign-in code on the game's own login-error screen when it has none. (Quest)
- **Windowed, no headset.** `-windowed` runs the game in a window with the headset checks patched out. (PC)
- **Works on Wine and Proton.** Wine is detected; the `_temp` directory fix and `-no-console` default apply. (PC) Tested clean: Proton Experimental 11.0 and Wine 11. Proton 8.0 (8.0-103) crashes in its own `dxgi.dll` right after the graphics device is created, with the stock `BugSplat64.dll` too, so use Proton Experimental or a recent Wine (#673). The boot log's `[NEVR.BOOT] host environment:` line records the Wine version.
- **A wrong `echovr.exe` is refused.** A wrong or damaged game image is refused instead of patched. (PC)

### Friends, party and the social lobby
- **Social lobby.** Log in and land in the social lobby through the community game service. (both)
- **Friends list.** Names with online, busy and offline presence, plus status text under each friend. (PC)
- **Friend changes without a restart.** Adds, accepts and removals made on the web site show in the game. (both)
- **Recently met.** The recently-met list and its refresh. (both)
- **Party.** A party exists after login; receive, accept and dismiss invites; promote a member; lock or unlock the party; join errors show the game's own popup. (both)
- **Send a party invite** from a friend row. (PC)
- **New accounts can invite.** The first-match gate reads as passed, so a fresh account can send a party invite. (both)

### Matchmaking and play
- **Matchmaker library host.** The matchmaker library's compiled service address is rewritten each time the library loads, because the game unloads and reloads it during a session. The patch is applied on every load ([#18](https://github.com/EchoTools/nevr-runtime/issues/18)). (PC)
- **Service redirect.** The game's hard-coded service addresses (HTTP and WebSocket) go to the community game service, over modern TLS 1.2/1.3. (PC)
- **Early-quit lockout.** The lockout countdown shows when the game service sends a penalty. (PC)
- **Arena rules in `config.yaml`.** Round time, celebration time and mercy score can be set under `arena.*`. (PC)

### Voice
- **Microphone on Wine and Proton.** A WASAPI capture provider feeds the game's own voice path, resampled to mono 48 kHz; it picks up the default capture device again after the device is invalidated. (PC)
- **Native Windows keeps the game's own microphone.** The runtime installs its provider under Wine and Proton only. (PC)

### Crash and exit handling
- **Crash recovery.** The game's main loop is wrapped, the original crash reporter is blocked, and a crash writes a readable dump and a crash record. (PC)
- **Clean exit.** Closing the window ends the process; Ctrl+C or a stop signal shuts down with a watchdog. (PC)
- **Guards.** Null session pointers and broadcaster listener tables that used to crash the game are guarded and counted. (PC)
- **Local minidump.** A Quest crash writes a minidump and the maps file locally. (Quest)

### Diagnostics
- **Structured logs.** One JSON line per event: `nevr-boot.jsonl` in `logs\` of the game folder, a timestamped `nevr-<timestamp>.jsonl` and the host's `nevr-bootstrap.jsonl` under `%LOCALAPPDATA%\nevr\echovr\logs` on PC (every PC file is listed in "Files the runtime uses"); logcat tag `NEVR-Sentinel` and `nevr-sentinel.log` on Quest.
- **The game's own log, filtered.** The built-in filter captures the game's log lines, folds repeats and reports its own health. (PC)
- **Hook accounting.** Each hook reports whether it was ever entered; overwritten hooks are detected; counters are reported once the game runs. (both)
- **Export tracing.** `-trace-exports` records the platform DLL calls. (PC)
- **Hardware dump.** A one-time hardware and environment dump file on request. (Quest)

### Game server mode
`echovr.exe -server -headless -no-console` boots, logs in, begins multiplayer, loads the in-process server library and registers with the game service. [`docs/testing/smoke-checklist.md`](docs/testing/smoke-checklist.md) (rows SV01 on) says how to check each item below.

- **Dedicated game server.** `-server` implies headless and no OVR; headless graphics need no GPU; `echovr_server.exe` starts one. (PC)
- **Registers with the game service.** Authenticates, registers (with guild and region filters), takes sessions and returns to the lobby after a round. (PC)
- **Operates unattended.** Re-registers after a dropped connection, exits after the session so a fleet manager can respawn it, shuts down cleanly on Ctrl+C, and holds an empty game server for a configurable time. (PC)
- **Network.** UPnP port mapping, internal and external address overrides, match telemetry streaming. (PC)

### Quest
- **A sentinel loaded with the game.** The redirect, the bridge, the login and the social façade follow from the embedded service endpoints and keys, with no switch of their own; `social.facade`, `social.send_presence_to_meta` and `diagnostics.hardware_dump` in `config.yaml` change the façade, the rich presence and the hardware dump (off by default). On every launch the sentinel answers the game's encrypted-OBB mount itself ([`docs/adr/0007-quest-obb-mount-skip.md`](docs/adr/0007-quest-obb-mount-skip.md)); the game then starts without the ~30 s wait (checked on a headset on 2026-10-16, recorded in [#572](https://github.com/EchoTools/nevr-runtime/issues/572)). (Quest)
- **Login and social on the headset.** The game's login is rewritten into the NEVR login, and the social facade decodes friends, presence, recently-met and party frames from the game service. (Quest)

### Known gaps
The issue tracker is the source of truth. Three things on Quest do not work yet: the main-menu
FRIENDS LIST ([#391](https://github.com/EchoTools/nevr-runtime/issues/391)), the party tab's
Invite Members ([#318](https://github.com/EchoTools/nevr-runtime/issues/318)), and the status text under
your name, which was fixed in code ([#393](https://github.com/EchoTools/nevr-runtime/issues/393)) but is kept
here until a passing run on a current build is recorded in that issue.

## Install for testers

1. **Sync the device clock first** (PC: Windows time sync; Quest: automatic time on), so
   PC and Quest logs line up afterwards.
2. **Windows:** take `nevr-runtime-v<X.Y.Z>-windows.zip` from the release (CI builds it from the tag `vX.Y.Z`),
   close the game, unzip it, and run `install.ps1` from PowerShell
   (`powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1`). The script runs `install-nevr-runtime.exe`, the
   core installer in the zip (`docs/adr/0023-core-install-method.md`). The core checks the signed release manifest,
   installs the two DLLs from the zip when their SHA-256 matches the manifest's entry for that release and downloads
   the package the manifest names when they are missing or differ, copies your original `BugSplat64.dll` to
   `BugSplat64.dll.original-<timestamp>` in the nEVR folder (`%LOCALAPPDATA%\nevr\echovr\installer\aside`), moves
   a legacy `dbgcore.dll` there as `dbgcore.dll.legacy-<date>` and installs the two files. The only file of ours in
   the game folder is `BugSplat64.dll`. It deletes nothing, puts the folder back as it was if a
   step fails, and writes a `[NEVR.INSTALL]` log whose path it prints.

   If you installed the beta by hand ([`docs/beta/INSTALL.md`](docs/beta/INSTALL.md)), the game folder already holds
   our `BugSplat64.dll`, and `install.ps1` cannot record the game's own file it replaced. Put the game's file back
   first: copy `%LOCALAPPDATA%\nevr\echovr\installer\aside\BugSplat64.dll.original` over `bin\win10\BugSplat64.dll`,
   or use Verify Files (Steam or the Meta Horizon app). Then run `install.ps1`. If neither is possible, `install.ps1`
   still installs, but it marks the game's file as unknown, and `uninstall.ps1` then refuses to run until the game's
   file is restored.
3. **Uninstall:** `uninstall.ps1 -Dir <bin\win10>` (plus `-Root <folder>` when `install.ps1` was given one) restores both and keeps the backups.
4. **Quest:** the Quest APK is not on GitHub: it repacks the store game. When a release has one (its release
   notes say so), it is at `https://evr.echo.taxi/nevr-runtime/v<X.Y.Z>/nevr-runtime-v<X.Y.Z>-quest.apk`, the
   address [`docs/reference/verifying-a-release.md`](docs/reference/verifying-a-release.md) gives; install it with
   `adb install`. A development build (`nevr-runtime-v<X.Y.Z-dev.N>-<sha7>-quest.apk`, built by `just package-dev`)
   is published outside GitHub, and this repo does not give its address. Uninstall the old version first; this
   build is signed with the new key.
5. Windows Defender or SmartScreen may warn about a file that carries no signature. `Get-AuthenticodeSignature .\BugSplat64.dll`
   (PowerShell) says whether a file is signed.

### Moving a Quest from the community installer to this APK

A headset whose Echo VR was installed by the community Quest installer cannot take this APK as an update. The
installer's game is signed with the installer's key, and Android installs an update only from the key that signed
the installed app, so `adb install -r` of this APK over it is refused with `INSTALL_FAILED_UPDATE_INCOMPATIBLE`.
There is one way over:

1. Uninstall Echo VR from the headset once (`adb uninstall com.readyatdawn.r15`, or from the headset's app library).
2. Install this APK with `adb install`.

This APK's `versionCode` is set when it is built, from its version (`tools/quest_apk_version.py`), and is always above
4987570, the game build the installer pins. So the installer's own build never replaces this one, and a later build of
this APK installs over an earlier one. The code is `major*10^7 + minor*10^5 + patch*10^3 + slot`; the slot is
the development build's distance N from the last release, or 999 for a release (a release `X.Y.Z` has `X*10^7 + Y*10^5 + Z*10^3 + 999`). It carries no
commit: two development builds from different branches at the same distance N have the same versionCode, so tell them
apart by the commit in the file name (`-<sha7>-quest.apk`).

What the code shows about the game data and the sign-in, and what it does not:

- With no OBB file, the game reads its data from `/storage/emulated/0/readyatdawn` and from no other folder
  ([ADR 0013](docs/adr/0013-quest-game-data-from-the-launcher.md)). A release build downloads nothing: when the data is
  not in that folder it shows "Echo VR game data is missing. Install Echo VR with Echo Launcher first, then start the
  game again." and ends. Data that sits only under `Android/media/com.readyatdawn.r15/files` is not read by a launch
  with no OBB file.
- The refresh token is kept in the app's private storage ([ADR 0003](docs/adr/0003-quest-networking-port.md)), which the
  uninstall removes, so the first launch of this APK starts the sign-in ([`docs/quest/SIGN-IN.md`](docs/quest/SIGN-IN.md))
  (to be confirmed on the headset, below).
- To be confirmed on a real installer-installed headset: which folder the installer puts the game data in, whether
  that folder is still there after the uninstall, whether the game starts after the move without a second data
  install, and what the first launch of this APK shows. The steps and predicted outcomes for that run are written
  down before it, in the headset check for the move.

The community beta guide for a manual install is [`docs/beta/INSTALL.md`](docs/beta/INSTALL.md).
The Quest sign-in flow is in [`docs/quest/SIGN-IN.md`](docs/quest/SIGN-IN.md).

## Files the runtime uses

Every path the Windows runtime reads or writes. An installer, an uninstaller or a support script builds to this
list. The **game folder** is the folder that holds `echovr.exe` (`<install>\bin\win10`); the code finds it with
`GetModuleFileName` on the game module. `%LOCALAPPDATA%` and `%TEMP%` are the Windows user's. Under Wine they
are inside the prefix (`<prefix>/drive_c/users/<user>/AppData/Local`).

### In the game folder

| Path | What it is | Code |
| --- | --- | --- |
| `BugSplat64.dll` | The file the game imports: the thin host, with the game's 28 `BugSplat64.dll` exports, the rendezvous detour and the crash capture. It loads `nevr-runtime.dll` and, when that cannot be loaded after attach, stops the game with exit code 78, after one plain-words dialog on a client (a server or headless run gets the `game_stop` record only). | `OUTPUT_NAME "BugSplat64"` of target `nevr_host` in `src/runtime/CMakeLists.txt`; `RuntimeRendezvous::StopGame` in `src/runtime/host/runtime_rendezvous.cpp` |
| `nevr-runtime.dll` | The runtime: hooks, modes, server, bridge, plugin loader and the statically linked modules. The host verifies it and loads it from the nEVR root (`%LOCALAPPDATA%\nevr\echovr`, ADR 0012), never from beside itself; `install.ps1` (through `install-nevr-runtime.exe`) puts it there. A dev build also reads `NEVR_ROOT` (an absolute path) for the runtime's location; a release build ignores it and logs that. | `OUTPUT_NAME "nevr-runtime"` of target `nevr_runtime` in `src/runtime/CMakeLists.txt`; `kRuntimeFileName` and `ResolveRuntimePath` in `src/runtime/host/runtime_load.cpp`, `src/core/nevr_root.h` |
| `BugSplat64.dll.original-<timestamp>`, `dbgcore.dll.legacy-<date>`, `nevr-install.txt` | What `install.ps1` (through `install-nevr-runtime.exe`) leaves, all in the nEVR root under `installer\` and `installer\aside\` and none in the game folder: the game's own `BugSplat64.dll`, a legacy `dbgcore.dll` set aside, a pre-existing `nevr-runtime.dll` in the nEVR root (`nevr-runtime.dll.original-<timestamp>`), a runtime an earlier release left beside `echovr.exe` (`nevr-runtime.dll.legacy-<timestamp>`), and the state file `nevr-install.txt` that `uninstall.ps1` reads (set aside as `nevr-install.txt.stale-<timestamp>` when `install.ps1` finds a record for a DLL that Verify Files has since restored). Both scripts append their runs to `nevr-install.log` beside the script, not in the game folder. | `tools/package-release/install.ps1`, `tools/package-release/uninstall.ps1` |
| `plugins\` | Plugin DLLs. A plugin loads only when the ordered `plugins:` list in `config.yaml` names it; a DLL the list does not name is ignored. | `LoadPlugins` (`pluginDir`) in `src/runtime/ext/plugin_loader.cpp` |
| `_overrides\` | Files registered as resource overrides at start. | `src/runtime/patch/resource_override.cpp` |
| `logs\nevr-boot.jsonl` | The boot log: one JSON line per event from before the log filter starts, appended across runs; the main log replays this run's lines. Created at start, with the `logs` folder. If the folder cannot be created or the file cannot be opened, the lines go to standard error only. | `nevr_boot_log_tee::Init` in `src/runtime/log/boot_log_tee.cpp` |
| `log_filter_config.yml`, `.yaml`, `.jsonc`, `.json` | Optional settings of the built-in log filter (`file_dir`, rotation). The first that exists is used, searched in this order, then in the working directory. | `FindConfigFile` in `src/runtime/log/builtin_filter.cpp` |

### In `_local`

`_local` is searched as `<game folder>\_local`, then `<game folder>\..\_local`, then `<game folder>\..\..\_local` (the
stock install keeps it beside `bin`). A `config.yaml` there is never read; see `config.yaml` in the nEVR root below.

| Path | What it is | Code |
| --- | --- | --- |
| `_local\config.json` | Optional. Only the keys the stock engine reads. | `LoadEarlyConfig` in `src/runtime/lifecycle/config.cpp` |
| `_local\r14logs\*.log` | The game's own log. The runtime only reads the newest, relative to the working directory. | `src/runtime/lifecycle/state_machine.cpp` |

### In `%LOCALAPPDATA%\nevr\echovr\`

| Path | What it is | Code |
| --- | --- | --- |
| `logs\nevr-bootstrap.jsonl` | The host's own log: what it decided at attach, and its crash-dump records. Falls back to `%TEMP%\nevr\echovr\logs`, then the directory of `BugSplat64.dll`. | `kLogsSubdirectory`, `kBootstrapLogName` in `src/runtime/host/host_paths.h` |
| `crashes\nevr-<pid>-<n>.dmp` | A minidump written by the host's crash capture, with the same fallbacks (`%TEMP%\nevr\echovr\crashes`, then the directory of `BugSplat64.dll`). The game's crash handler reaches it through the exported `BugSplat_createReport` in `src/runtime/host/minidmp_sender_exports.cpp`. | `kCrashesSubdirectory` in `src/runtime/host/host_paths.h`; `CrashCapture::Capture` in `src/runtime/host/crash_capture.cpp` |
| `config.yaml` | Every nEVR setting: service addresses and keys, the `plugins:` list, arena rules. Read from the file `-config-file <file>` names, else from this folder; nothing else is searched. A `-config-file` that names no readable file ends the run with exit code 64 and one line on standard error. With no file the run uses the built-in defaults and logs `no config.yaml found; searched: <path>` (KNOWN-WRONG #1254: a server with no file and no cached sign-in never stops); a malformed file stops a dedicated server. A `config.yaml` in `<game folder>\_local`, `..\_local` or `..\..\_local` is never read: when this folder has none, the first of those is copied here once and the old file is left as it is; when this folder has one, the old file is ignored with a warning. | `nevr_config_yaml::Choose` in `src/runtime/lifecycle/config_yaml_choice.h`; `nevr_config_yaml::Resolve` in `src/runtime/lifecycle/config_yaml_source.cpp`; `NevrCfg` in `src/runtime/lifecycle/service_config.cpp` |
| `.credentials.json` | The cached refresh token and identity (the access token is never written), one per machine for clients and servers alike. A write goes to `.credentials.json.tmp` first, then replaces the cache. The first run moves a cache out of `<game folder>\_local`, `..\_local` or `..\..\_local` (the nearest wins) through the same protected write, into a temp file of its own, and installs it only when the root has no cache (a cache already there is never replaced or deleted); a cache the move could not take is still read, and written back where it is, until a later run moves it. Without `LOCALAPPDATA`, or when the root's path does not fit the ANSI code page, the cache stays in the first of those three that holds a `.credentials.json`, else the first that holds a `config.yaml`, else `<game folder>\_local`. | `SelectCredentialCacheLocation` in `src/core/auth_token.h`; `TempPathFor`, `UniqueTempPathFor` and `InstallCacheIfAbsent` in `src/core/credential_write.h`; `MoveCredentials` in `src/runtime/lifecycle/nevr_root_move.cpp` |
| `cache\<sha256>.blob`, `cache\index.json` | The content-addressed asset cache: the bytes under their own SHA-256, and the index that says which blob stands in for which resource. A write in progress is `<sha256>.<pid>.<tick>.tmp`. The cache is size-capped and evicts blobs the index does not name. Without `LOCALAPPDATA` there is no cache directory: the cache is disabled and the runtime logs `[NEVR.RESOURCE] cache disabled reason=no cache directory (LOCALAPPDATA unset)`. | `src/runtime/patch/asset_cache.h`, `DefaultDir` in `src/runtime/patch/asset_cache.cpp` |
| `logs\nevr-<timestamp>.jsonl` | The main run log (the built-in filter's output). Rotated by size and age; the newest `rotate_keep` files are kept. Without `LOCALAPPDATA` it is `logs\` beside the game's executable. The `file_dir` setting of the filter config replaces the folder in both cases. | `GetDefaultLogDir`, `BuildLogFilePath` in `src/runtime/log/builtin_filter.cpp` |
| `logs\nevr-crash-<run id>.txt` | The crash record: the `[NEVR.CRASH]` lines, written only once a crash path is entered. Without `LOCALAPPDATA` (or when its value is `MAX_PATH` characters or longer) it is `logs\nevr-crash-<run id>.txt` beside the game's executable. Neither case creates the `logs` folder: if it does not exist the file is not written. | `PrepareCrashRecordPath` in `src/runtime/lifecycle/crash_recovery.cpp` |
| `plugins\` | Plugin DLLs, their dependencies and data files. A plugin loads only when the ordered `plugins:` list in `config.yaml` names it; a DLL the list does not name is ignored. Without `LOCALAPPDATA` there is no plugin directory: every configured plugin fails with `the plugin directory cannot be resolved` and nothing is searched in the game folder. The first run moves the files of a `plugins\` folder beside `echovr.exe` here. | `LoadPlugins` in `src/runtime/ext/plugin_loader.cpp`; `PluginsDir` in `src/core/nevr_root.h`; the plugins source in `Run` in `src/runtime/lifecycle/nevr_root_move.cpp` |
| `cosmetics\v1\packages\` | Downloaded cosmetic packages (client only). The folder is created on demand; without `LOCALAPPDATA`, or if the creation fails, the error is logged and no package is stored. | `GetCacheDir` in `src/runtime/patch/asset_cdn.cpp` |

On the first run the runtime moves files it finds in the old places (the previous log, crash record, cosmetics, plugins and sign-in cache locations) into this folder and leaves `MOVED-TO-NEVR-ROOT.txt` in each old folder under `%LOCALAPPDATA%` and `_local` it emptied something from. The plugins folder beside `echovr.exe` gets no such file: nothing of ours is written to the game folder.

## What gets built

### The two DLLs

**`src/runtime/host/` → `BugSplat64.dll`** (the host) and the rest of **`src/runtime/` → `nevr-runtime.dll`**
(the runtime). Both go in the game folder.

The game statically imports `BugSplat64.dll` (the original crash reporter), so
replacing it gives the earliest possible hook point — it loads before `WinMain`.
The host is deliberately small: the game's 28 `BugSplat64.dll` exports, the crash capture
and the one hook on the game's main wrapper, where it loads `nevr-runtime.dll`
and hands the game over to it. The runtime carries the boot hooks, CLI flags, headless/server mode patches, crash
recovery, config and service redirection, the module and plugin loaders, the
built-in log filter, and the in-process game server
(`src/runtime/server/`).

Hooking is [MinHook](https://github.com/TsudaKageyu/minhook)-based.

### Command-line arguments

Arguments the runtime reads from the game's command line (`--name` works as well as `-name`).

| Argument | Effect |
| -------- | ------ |
| `-server` | Run as a dedicated game server (implies headless) |
| `-offline` | Run the game in offline mode |
| `-windowed` | Run the game with no headset, in a window |
| `-headless` | Run with no window: the headless gates are applied, as `-server` applies them |
| `-noovr` | Run with OVR (the headset runtime) turned off, the game's own flag |
| `-no-console` | Disable console window creation |
| `-config` | Path to a JSON file the game loads instead of `_local/config.json`; it has no effect on where `config.yaml` is read |
| `-config-file` | Path to the `config.yaml` to read instead of the one in the nEVR root; a file that is missing or cannot be opened ends the run with exit code 64 |
| `-region` | Set the matchmaking region |
| `-serverregion` | Set the server fleet region |
| `-no-exit-on-error` | Keep a server running after a game service disconnect (default: exit) |
| `-exit-on-error` | No effect: exiting on error is the default |
| `-trace-exports` | Trace the export calls of platform DLLs (`pnsrad`, `pnsovr`, `pnsdemo` or `all`); off by default |
| `-no-telemetry` | Disable telemetry streaming |
| `-telemetry-rate` | Set the telemetry rate in Hz (default 10) |
| `-telemetry-diag` | Log telemetry diagnostics every second |
| `-timestamps` | Prefix log lines with timestamps |
| `-upnp` | Enable UPnP port forwarding |
| `-require-signed-plugins` | Load only plugins signed through the release pipeline and refuse the rest. Without it, plugins load whether or not they are signed. The `plugin trust:` line in the log states which applied |

### Runtime-loaded modules

The `platform_compat` and `token_auth` modules are statically linked into
`nevr-runtime.dll`; there are no separate module DLLs to deploy. They use
`NvrModuleContext`, not the plugin interface.

| Module | Linked into | Purpose |
| ------ | ----------- | ------- |
| `platform-compat` | `nevr-runtime.dll` | Schannel TLS modernization, MSXML6 pass-through hook, Wine `_temp` fix |
| `token-auth` | `nevr-runtime.dll` | Device-code auth, token cache |

The bridge is why the game never negotiates TLS for its WebSocket traffic: it
speaks plaintext to a local proxy, and the proxy terminates TLS outbound.

### Plugins

Optional, loaded from `plugins\` in the nEVR root (`%LOCALAPPDATA%\nevr\echovr`, ADR 0012) via the `NvrPluginInterface` lifecycle.
A plugin loads only when the ordered `plugins:` list in `config.yaml` names it; a DLL the list does not name is ignored
(`LoadPlugins` in `src/runtime/ext/plugin_loader.cpp`). A `plugins\` folder left beside `echovr.exe` by an earlier release is moved into the root on the first run.

A plugin builds against the plugin SDK in `sdk/`: the header-only CMake target
`nevr::plugin_sdk`, with the plugin C ABI (`nevr/plugin_interface.h`) and the
plugin-side helpers. `cmake -S sdk -B <dir> && cmake --install <dir> --prefix <prefix>`
installs it for `find_package(NevrPluginSdk <api> CONFIG)`; FetchContent with
`SOURCE_SUBDIR sdk` adds it from a source tree. `nevr/*.h` builds for PC and Quest;
`nevr/pc/*.h` is Windows only.

Gameplay and tooling plugins live in the separate `nevr-runtime-plugins`
repository. The loader rejects `log_filter.dll`, because its filter is built
into the runtime.

### Other targets

- `src/nevr_api/` — protobuf, generated into `gen/cpp/` (`just proto`)
- `src/abi/` → `libnevr_abi.a` — the echovr.exe ABI surface: game types, function
  pointers, symbol IDs
- `src/core/` → `libnevr_core.a` — our own primitives: logging, globals, base64,
  hooking, auth-token model, `pch.h` (links `nevr_abi`)
- `sdk/` → `nevr::plugin_sdk` — the plugin SDK: the plugin C ABI, the CSymbol64 hash
  and the plugin-side helpers, header-only
- `src/extension/` — header-only published C ABI for the static modules and the scripting host
- `src/launcher/` → `echovr_server.exe`, a `CreateProcess` wrapper spawning
  `echovr.exe -server -headless -no-console` (a dedicated game server); its window shows the server's run log
  (`%LOCALAPPDATA%\nevr\echovr\logs`) until the server exits
- `src/libovr-stub/` → `LibOVRPlatform64_1.dll` — Oculus platform stub, built
  separately from the community beta package
- `src/quest/` — separate Android/Quest arm64 target

## Building

Requires CMake 4.0+, Ninja, and MinGW (`x86_64-w64-mingw32-g++`) to
cross-compile from Linux. Dependencies come from the vcpkg manifest.

```sh
just                # list recipes
just build          # build desktop targets
just verify         # the full gate (main, release prep): build + tests under Wine + invariant sensors
just test-system-short        # quick Go system tests (needs the game binary)
just test-plugins-groundtruth # plugin tests without the game binary
just test-android              # Quest binary-shape checks
just dist           # the stripped release package (symbols split to dist/symbols/); a build with symbols is build/<preset>/bin/
```

Presets: `mingw-debug`, `mingw-release` (Linux default), `debug`, `release`
(Windows). Output lands in `build/<preset>/bin/`.

`just verify` is the closed-loop gate for main after the release cut (a PR runs the quick check, see `AGENTS.md`): it runs the build, C++ tests under Wine,
Python harness tests, and source-invariant sensors.

## Deployment

From `build/mingw-release/bin/`:

| Artifact | Destination |
| -------- | ----------- |
| `BugSplat64.dll` | game directory (replaces the crash reporter; the host) |
| `nevr-runtime.dll` | the nEVR root, `%LOCALAPPDATA%\nevr\echovr` (the runtime; `BugSplat64.dll` stays in the game directory) |
| `echovr_server.exe` | game directory, alongside `echovr.exe` |
| Optional plugin DLLs | `plugins\` in the nEVR root, `%LOCALAPPDATA%\nevr\echovr` |

The staged server package and its `.tar.zst` carry an empty `plugins/` directory
for optional operator plugins; the Windows zip (`nevr-runtime-v<ver>-windows.zip`)
has no entry for it, so create `plugins\` in the nEVR root (the `plugins/` of the server package is not read). Plugin DLLs are
built separately and are not bundled. The community beta install guide covers installing the game client and
uses `BugSplat64.dll` and `nevr-runtime.dll`.

For the community beta on Windows, follow [`docs/beta/INSTALL.md`](docs/beta/INSTALL.md).

## Repository layout

```
src/runtime/   nevr-runtime.dll — hooks, modes, crash recovery, gameserver; src/runtime/host/ is BugSplat64.dll
src/modules/       statically linked module implementations (platform-compat, token-auth)
src/abi/           libnevr_abi.a  — the echovr.exe ABI surface
src/core/          libnevr_core.a — our own primitives (links nevr_abi)
src/extension/     header-only C ABI of the static modules and the scripting host
src/nevr_api/      protobuf target
sdk/               the plugin SDK (nevr::plugin_sdk), header-only
tools/             build and verify tooling
tests/             Go system suites (not part of `just verify`)
extern/            submodules: minhook, breakpad, lss
gen/               generated protobuf — do not hand-edit
docs/              see docs/README.md
```

## Documentation

| | |
| - | - |
| [`AGENTS.md`](AGENTS.md) | Project conventions, build/test commands, and agent guardrails |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Build, verify, and submit a change |
| [`LICENSE`](LICENSE) | Apache License 2.0 |
| [`NOTICE`](NOTICE) | Bundled third-party software notices |
| [`SECURITY.md`](SECURITY.md) | Private vulnerability reporting |
| [`docs/`](docs/) | Standards, guides, reference, design, audits |
| [`docs/reference/verifying-a-release.md`](docs/reference/verifying-a-release.md) | How an installer verifies a release: signed manifest, pinned keys, artifacts |

## Dependencies

vcpkg manifest (`vcpkg.json`): curl, gtest, ixwebsocket, nlohmann-json,
miniupnpc, minhook, opus, protobuf.

Submodules in `extern/`: `minhook`, `breakpad`, `lss`.

## Related projects

| Project | Description |
| ------- | ----------- |
| **nevr-runtime** (this repo) | Runtime patches for `echovr.exe` |
| **nevr-runtime-plugins** | Gameplay and tooling plugins |
| **nakama** | The community game service (echovrce) |
| **revault** | Reverse-engineering data warehouse for the game binaries |

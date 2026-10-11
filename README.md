# nEVR Runtime

nEVR Runtime keeps Echo VR (`echovr.exe`) playable on the community game service
([echovrce](https://github.com/echotools), Nakama-based). Drop one file,
`BugSplat64.dll`, into the game folder and the game signs in with your Discord
account, reaches the social lobby and shows its friends and party. The Quest
build does the same through a repacked APK. For contributors it is the C++17
source of that DLL and of the Quest sentinel, plus the tooling and tests that
keep both honest.

Part of the **NEVR** project, keeping Echo VR alive.

> Working in this repo as an agent? Start at [`AGENTS.md`](AGENTS.md).

## What it does

PC means the Windows or Wine game client, Quest means the Android sentinel, both
means each. Every feature below is on `main`; [`docs/testing/smoke-checklist.md`](docs/testing/smoke-checklist.md)
says how to check each one and which log line proves it.

### Get in
- **Nothing to configure.** The service endpoints and the public client keys are built in, so a game client with a build-embedded configuration signs in with no `config.yaml` and no `nevr-quest.json`. `config.yaml` (PC) and `nevr-quest.json` (Quest) override them. (both)
- **Sign in with Discord.** A device-code sign-in in your browser; the saved sign-in is reused on the next start and refreshed in the background. (PC)
- **Saved sign-in on the headset.** The Quest build reuses its saved sign-in, and shows a sign-in code on the game's own login-error screen when it has none. (Quest)
- **Windowed, no headset.** `-windowed` runs the game in a window with the headset checks patched out. (PC)
- **Works on Wine and Proton.** Wine is detected; the `_temp` directory fix and `-noconsole` default apply. (PC)
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
- **Matchmaker library host.** The matchmaker library's compiled service address is rewritten each time the library loads, because the game unloads and reloads it during a session. The live check that the PUBLIC MATCH terminal screen populates is still open ([#18](https://github.com/EchoTools/nevr-runtime/issues/18)). (PC)
- **Service redirect.** The game's hard-coded service addresses (HTTP and WebSocket) go to the community game service, over modern TLS 1.2/1.3. (PC)
- **Early-quit lockout.** The lockout countdown shows when the game service sends a penalty. (PC)
- **Arena rules in `config.yaml`.** Round time, celebration time and mercy score can be set under `arena.*`. (PC)

### Voice
- **Microphone on Wine and Proton.** A WASAPI capture provider feeds the game's own voice path, resampled to mono 48 kHz; it picks up the default capture device again after the device is invalidated. (PC)
- **Native Windows keeps the game's own microphone.** The runtime installs its provider under Wine and Proton only. (PC)

### Crash and exit handling
- **Crash recovery.** The game's main loop is wrapped, the original crash reporter is blocked, and a crash writes a readable dump and a crash record. (PC)
- **Clean exit.** Closing the window ends the process; Ctrl+C or a stop signal shuts down with a watchdog. (PC)
- **Guards.** Null session pointers and entity lookups that used to crash the game are guarded and counted. (PC)
- **Local minidump.** A Quest crash writes a minidump and the maps file locally. (Quest)

### Diagnostics
- **Structured logs.** One JSON line per event: `nevr-boot.jsonl` and a timestamped `nevr-<timestamp>.jsonl` under `%LOCALAPPDATA%\EchoVR\logs` on PC; logcat tag `NEVR-Sentinel` and `nevr-sentinel.log` on Quest.
- **The game's own log, filtered.** The built-in filter captures the game's log lines, folds repeats and reports its own health. (PC)
- **Hook accounting.** Each hook reports whether it was ever entered; overwritten hooks are detected; counters are reported once the game runs. (both)
- **Export tracing.** `-traceexports` records the platform DLL calls. (PC)
- **Hardware dump.** A one-time hardware and environment dump file on request. (Quest)

### Game server mode
The dedicated game server does not complete bring-up today ([#45](https://github.com/EchoTools/nevr-runtime/issues/45)): `echovr.exe -server -headless -noconsole` boots, logs in and joins the social lobby group, then never reaches dedicated-server bring-up. The bullets below are what the code implements; none of it is shown working end to end.

- **Dedicated game server.** `-server` implies headless and no OVR; headless graphics need no GPU; `echovr_server.exe` starts one. (PC)
- **Registers with the game service.** Authenticates, registers (with guild and region filters), takes sessions and returns to the lobby after a round. (PC)
- **Operates unattended.** Re-registers after a dropped connection, exits after the session so a fleet manager can respawn it, shuts down cleanly on Ctrl+C, and holds an empty game server for a configurable time. (PC)
- **Network.** UPnP port mapping, internal and external address overrides, match telemetry streaming. (PC)

### Quest
- **A sentinel loaded with the game.** Four switchable features in `nevr-quest.json`: `redirect`, `bridge`, `login`, `social`; `obb_skip` and `hwdump` are separate and off by default. (Quest)
- **Login and social on the headset.** The game's login is rewritten into the NEVR login, and the social facade decodes friends, presence, recently-met and party frames from the game service. (Quest)

### Known gaps
The issue tracker is the source of truth. On Quest the main-menu FRIENDS LIST
([#391](https://github.com/EchoTools/nevr-runtime/issues/391)), QUIT
([#392](https://github.com/EchoTools/nevr-runtime/issues/392)), the status text under your
name ([#393](https://github.com/EchoTools/nevr-runtime/issues/393)) and the party tab's
Invite Members ([#318](https://github.com/EchoTools/nevr-runtime/issues/318)) do not work
yet. On PC:

- the dedicated game server does not complete bring-up
  ([#45](https://github.com/EchoTools/nevr-runtime/issues/45));
- the main menu shows no game service status, because the status request fails
  ([#408](https://github.com/EchoTools/nevr-runtime/issues/408));
- the matchmaking screen's time remaining is always 0
  ([#414](https://github.com/EchoTools/nevr-runtime/issues/414));
- under Wine and Proton the microphone ring buffer overflows at the start of a capture and
  whether the game reads it at real time is still being measured
  ([#95](https://github.com/EchoTools/nevr-runtime/issues/95));
- the party roster keeps a member who disconnected
  ([#403](https://github.com/EchoTools/nevr-runtime/issues/403));
- party data sharing is refused
  ([#398](https://github.com/EchoTools/nevr-runtime/issues/398));
- a friend request shows no prompt to the receiver
  ([#405](https://github.com/EchoTools/nevr-runtime/issues/405));
- the PUBLIC MATCH terminal screen after a matchmaker library reload has not been checked live
  ([#18](https://github.com/EchoTools/nevr-runtime/issues/18)).

## Install for testers

1. **Sync the device clock first** (PC: Windows time sync; Quest: automatic time on), so
   PC and Quest logs line up afterwards.
2. **Windows:** take `nevr-runtime-v<X.Y.Z>-windows.zip` from the release (CI builds it from the tag `vX.Y.Z`),
   close the game, unzip it, and run `install.ps1` from PowerShell
   (`powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1`). It checks the package
   against `SHA256SUMS`, copies your original `BugSplat64.dll` to
   `BugSplat64.dll.original-<timestamp>`, renames a legacy `dbgcore.dll` to
   `dbgcore.dll.legacy-<date>` and installs the new file. It deletes nothing.
3. **Uninstall:** `uninstall.ps1 -Dir <bin\win10>` restores both and keeps the backups.
4. **Quest:** a release is the Windows zip only: the Quest APK repacks the store game and is not part of
   the public set. Testers receive a development build
   (`nevr-runtime-v<X.Y.Z-dev.N>-<sha7>-quest.apk`, from `just package-dev`) privately; install it with
   `adb install -r`. It is signed with the same key as earlier test builds, so it installs over them and
   keeps your data.
5. The Windows DLL is **unsigned**; Windows Defender or SmartScreen may warn about it.

The community beta guide for a manual install is [`docs/beta/INSTALL.md`](docs/beta/INSTALL.md).
The Quest sign-in flow is in [`docs/quest/SIGN-IN.md`](docs/quest/SIGN-IN.md).

## What gets built

### The main DLL

**`src/runtime/` → `BugSplat64.dll`**, deployed under that same name.

The game statically imports `BugSplat64.dll` (the original crash reporter), so
replacing it gives the earliest possible hook point — it loads before `WinMain`.
It carries the boot hooks, CLI flags, headless/server mode patches, crash
recovery, config and service redirection, the module and plugin loaders, the
built-in log filter, and the in-process game server
(`src/runtime/server/`).

Hooking is [MinHook](https://github.com/TsudaKageyu/minhook)-based.

### Runtime-loaded modules

The `platform_compat` and `token_auth` modules are statically linked into
`BugSplat64.dll`; there are no separate module DLLs to deploy. They use
`NvrModuleContext`, not the plugin interface.

| Module | Linked into | Purpose |
| ------ | ----------- | ------- |
| `platform-compat` | `BugSplat64.dll` | Schannel TLS modernization, MSXML6 pass-through hook, Wine `_temp` fix |
| `token-auth` | `BugSplat64.dll` | Device-code auth, token cache |

The bridge is why the game never negotiates TLS for its WebSocket traffic: it
speaks plaintext to a local proxy, and the proxy terminates TLS outbound.

### Plugins

Optional, discovered from a `plugins/` directory next to the game binary and
loaded via the `NvrPluginInterface` lifecycle.

| Plugin | Output | Purpose |
| ------ | ------ | ------- |
| `example` | `nevr_example.dll` | Reference implementation for new plugin authors |
| `debug-lockout` | `nevr_debug_lockout.dll` | Debug instrumentation for early-quit lockout state |

Gameplay and tooling plugins live in the separate `nevr-runtime-plugins`
repository. `log-filter` source remains in `plugins/log-filter/`, but it is not
built or packaged; the loader rejects `log_filter.dll` because its filter is
already built into the runtime.

### Other targets

- `src/nevr_api/` — protobuf, generated into `gen/cpp/` (`just proto`)
- `src/abi/` → `libnevr_abi.a` — the echovr.exe ABI surface: game types, function
  pointers, symbol IDs, CSymbol64 hashing
- `src/core/` → `libnevr_core.a` — our own primitives: logging, globals, base64,
  hooking, auth-token model, `pch.h` (links `nevr_abi`)
- `src/extension/` — header-only published C ABI for third-party plugins/modules
- `src/launcher/` → `echovr_server.exe`, a `CreateProcess` wrapper spawning
  `echovr.exe -server -headless -noconsole` (a dedicated game server)
- `src/libovr-stub/` → `LibOVRPlatform64_1.dll` — Oculus platform stub, built
  separately from the community beta package
- `src/quest/` — separate Android/Quest arm64 target
- `src/legacy/` — **frozen** v1 implementations, self-contained; do not modify

## Building

Requires CMake 4.0+, Ninja, and MinGW (`x86_64-w64-mingw32-g++`) to
cross-compile from Linux. Dependencies come from the vcpkg manifest.

```sh
just                # list recipes
just build          # build desktop targets
just verify         # THE GATE — build + tests under Wine + invariant sensors
just test-system-short        # quick Go system tests (needs the game binary)
just test-plugins-groundtruth # plugin tests without the game binary
just test-android              # Quest binary-shape checks
just dist           # distribution packages
just dist-lite      # stripped, no debug symbols
```

Presets: `mingw-debug`, `mingw-release` (Linux default), `debug`, `release`
(Windows). Output lands in `build/<preset>/bin/`.

`just verify` is the closed-loop gate: it runs the build, C++ tests under Wine,
Python harness tests, and source-invariant sensors.

## Deployment

From `build/mingw-release/bin/`:

| Artifact | Destination |
| -------- | ----------- |
| `BugSplat64.dll` | game directory (replaces the crash reporter) |
| `echovr_server.exe` | game directory, alongside `echovr.exe` |
| Optional plugin DLLs | `plugins/` next to the game binary |

The distribution package includes an empty `plugins/` directory for optional
operator plugins; plugin DLLs are built separately and are not bundled. The
community beta install guide covers installing the game client and uses only
`BugSplat64.dll`.

For the community beta on Windows, follow [`docs/beta/INSTALL.md`](docs/beta/INSTALL.md).

## Repository layout

```
src/runtime/   BugSplat64.dll — hooks, modes, crash recovery, gameserver
src/modules/       statically linked module implementations (platform-compat, token-auth)
src/abi/           libnevr_abi.a  — the echovr.exe ABI surface
src/core/          libnevr_core.a — our own primitives (links nevr_abi)
src/extension/     header-only C ABI published to third-party DLLs
src/nevr_api/      protobuf target
src/legacy/        FROZEN v1
src/legacy-compat/ two forwarding headers; frozen legacy needs them
plugins/           optional plugins
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

## Local configuration

The root `CMakeLists.txt` does not include `cmake/local.cmake` automatically.
To use that file, enable its include in `CMakeLists.txt`; then a local
post-build command can copy the DLL into a game directory:

```cmake
set(GAME_DIR "/path/to/echovr/bin/win10")
add_custom_command(TARGET nevr_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nevr_runtime> "${GAME_DIR}/BugSplat64.dll")
```

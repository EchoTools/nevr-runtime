# Launcher contract: the install root

What a launcher or installer does to put nEVR on a PC, what the host does at game start, and what the launcher can read afterwards.
Design and reasons are in `docs/adr/0012-static-install-root.md`; this page is the contract of the tree. Where a thing is still in
the game folder it says so, with the issue that moves it.

## What goes where

| File | Where it goes | What it is |
| --- | --- | --- |
| `BugSplat64.dll` | the game's `bin\win10\`, replacing the game's own file of that name | the host: verifies and loads the runtime, writes the host log, captures crashes |
| `nevr-runtime.dll` | `%LOCALAPPDATA%\nevr\echovr\` of the Windows account that runs the game | the runtime |

Nothing else of ours is put in the game folder by a launcher. The root, `%LOCALAPPDATA%\nevr\echovr`, is defined once in
`src/core/nevr_root.h` (`nevr_root::Names`); under Wine it is the Wine prefix's own `%LOCALAPPDATA%`. The folder is made by the
launcher if it is missing. The game's own `%LOCALAPPDATA%\EchoVR` is never written by nEVR.

The root also holds what the runtime writes: `logs\`, `crashes\`, `.credentials.json`, `cache\`, `cosmetics\v1\packages\`.
A launcher does not create or edit those. Plugin files go in the root's `plugins\` (`%LOCALAPPDATA%\nevr\echovr\plugins\`); the runtime loads plugins from there and never from beside `echovr.exe`. The first run moves the files of an old `plugins\` folder beside `echovr.exe` into it, leaves no file behind, and logs each move.

## What the host does at game start

The host resolves `<root>\nevr-runtime.dll` (`nevr_host::ResolveRuntimePath`, `src/runtime/host/runtime_load.cpp`). There is no
fallback: a `nevr-runtime.dll` beside `BugSplat64.dll` is not looked at.

Before it maps the file the host verifies it (`RuntimeLoader::Load`): the signature must chain to the pinned Microsoft root with the
project's profile EKU (`src/runtime/ext/plugin_pins.cpp`, ADR 0010). The file is held open for reading across the load. A release
host accepts only a signed runtime. A dev build (`host_build` `dev` in the `versions` record) accepts an unsigned one and honours
`NEVR_ROOT`, which replaces the root for the host's search for the runtime only and must be an absolute path; a release host
ignores it and says so in the `runtime_load` record (`root_override` `ignored`). The runtime's own state paths (logs, credentials, cache) always use
`%LOCALAPPDATA%` and never `NEVR_ROOT`.

When the runtime does not load, the game stops. There is no run without nEVR:

- the process ends with exit code 78 (`TerminateProcess` from the host, `src/runtime/host/`);
- a dialog says what is wrong (`StopNoticeTextFor` in `src/runtime/host/stop_notice.h`: missing, unsigned, changed, unreadable), unless the run is
  `-server`, `-headless` or the loader lock is held, which write the record only (`notice` in the `game_stop` record);
- the stop reasons are the `outcome` values of `RuntimeLoadResult::OutcomeName`: `no_path`, `runtime_absent`, `dependency_load_failed`,
  `load_failed`, `missing_entry`, `null_descriptor`, `abi_rejected`, `init_failed`, `runtime_refused`. A runtime that was never
  installed in the root is `runtime_absent`; a file the verifier does not accept is `runtime_refused`.

## What a launcher can read

The host log is `%LOCALAPPDATA%\nevr\echovr\logs\nevr-bootstrap.jsonl` (`%TEMP%\nevr\echovr\logs` when the root cannot be made;
`src/runtime/host/host_paths.h`). It is JSON Lines, one object per line, each with `tag` `NEVR.HOST`, `ts`, `pid` and `run`. A launcher
that wants to know why the game did not start reads the newest `run`'s records:

| `event` | Fields a launcher can use |
| --- | --- |
| `versions` | `host_version`, `host_build` (`release` or `dev`), `host_sha256`, `runtime_version`, `runtime_sha256` (both `unavailable` when the runtime did not load), `runtime_path` |
| `runtime_load` | `outcome` (above), `root` (`local_appdata` or `nevr_root_override`), `verify` (`accepted`, `unsigned_allowed` or `refused`), `verify_reason`, `path`, `abi_status`, `degraded_action` (`dispatch` or `stop_game`) |
| `game_stop` | `reason`, `exit_code` (78), `notice` (`shown`, `suppressed_server_or_headless` or `suppressed_loader_lock`), `runtime_path`, `host_identity` |

A run that logged in has, in the runtime's log in the same `logs\` folder (`nevr-<date>T<time>.jsonl`), the lines
`[NEVR.WS] LOGIN SUCCESS` and `NetGame switching state (from logging in, to logged in)`.

## Flags a launcher may pass

The runtime reads these from the game's command line (`src/core/cli_flags.h`, `src/runtime/lifecycle/cli.cpp`):

- `-config-file <file>`: the exact `config.yaml` to read. Without it the runtime reads `config.yaml` from the root, and nothing else is
  searched. A file that is missing or cannot be opened ends the run with exit code 64 and one JSON line on standard error
  (`"tag":"NEVR.STOP"`, `"reason":"usage"`, `"detail"` naming the argument and why); `-config-path` is refused the same way with
  `use -config-file <file>`. `-config <file>` is the game's own JSON file and does not affect where `config.yaml` is read.
- `-require-signed-plugins`: the run loads only signed plugins (ADR 0010). Without it the run loads unsigned plugins and logs a
  warning for each; a launcher that wants the signed-only mode passes it.

## What the scripts in the package do

`install.ps1` and `uninstall.ps1` (`tools/package-release/`) are the reference implementation of this contract. `install.ps1`
takes `-Dir <...\bin\win10>` and `-Root <nevr folder>` (an absolute path; the root is `%LOCALAPPDATA%\nevr\echovr` when omitted):

- it refuses while `echovr.exe` runs, when the package's `SHA256SUMS` does not match, and when `BugSplat64.dll` in the game folder is a
  nEVR runtime it has no record of;
- it copies the game's own `BugSplat64.dll` to `<root>\installer\aside\BugSplat64.dll.original-<timestamp>` before it writes the host,
  and moves a legacy `dbgcore.dll` (`dbgcore.dll.legacy-<date>`), a `nevr-runtime.dll` left beside `echovr.exe`
  (`nevr-runtime.dll.legacy-<timestamp>`) and an `nevr-install.txt` left beside `echovr.exe` by an earlier installer
  (`nevr-install.txt.legacy-<timestamp>`) into the same folder; it deletes nothing of the player's;
- it writes `nevr-runtime.dll` into the root and `BugSplat64.dll` into the game folder, and records what it did in
  `<root>\installer\nevr-install.txt` (`backup`, `legacy`, `runtime_aside`, `runtime_legacy`, `runtime_root`,
  `installed_sha256`, `runtime_sha256`, `owner`, and, once an install has been updated, `previous_sha256`, the hashes of the hosts it replaced);
- run again from a newer package it updates both files in place.

`uninstall.ps1` takes `-Dir` and, when the install was given one, the same `-Root`. It reads that record (a record an earlier
installer left beside `echovr.exe` is read when the root has none), restores the game's `BugSplat64.dll`, sets the root's
`nevr-runtime.dll` aside as `<root>\installer\aside\nevr-runtime.dll.uninstalled-<timestamp>`, puts back a runtime it moved out
of the game folder, and moves the record to `<root>\installer\aside\nevr-install.txt.undone-<timestamp>`.

## What still writes in the game folder

These are not yet in the root (#575). Each moves in its own change and this page is updated with it:

- the game's own logs under `_local\r14logs` (the game writes them);
- `logs\nevr-boot.jsonl` beside `echovr.exe`: the runtime's start-up log, KNOWN-WRONG #1232;

## Who owns an install

An install has one owner. Two things say who:

- **The launcher's own state.** Echo VR Launcher keeps `launcher.json` in `%LOCALAPPDATA%\EchoVR_Launcher`; each `versions[]` entry
  names an install root, and the game's folder is `<root>\ready-at-dawn-echo-arena\bin\win10` (`bin\win7` for the 2017 builds).
  `nevr-install pc install` (and so `install.ps1`) reads that file, never writes it, and refuses a game folder it lists:
  "Echo VR Launcher manages this install (nEVR <version> is installed; ...). Update nEVR through the launcher." The run ends
  `needs-user`, changes nothing and fetches no package. `--launcher-dir <folder>` names the launcher's data folder when it is not
  the default (the default exists on Windows only). A `launcher.json` that is there and cannot be read is a refusal too, because
  the check cannot say the launcher is absent; a missing one means no launcher.
- **The `owner` key of `nevr-install.txt`.** The core writes `owner=nevr-install`. An installer that manages the same folder
  writes its own name (the launcher: `owner=launcher`); `install.ps1` and `uninstall.ps1` leave a record with any other owner
  alone and say so. A record with no `owner` key, written before the key existed, is ours.

A folder neither says is owned by anyone: an nEVR host in it with no record is adopted by the install (the game's own file is then
marked `original=unknown`).

## What a launcher must not do

- Put `nevr-runtime.dll` beside `BugSplat64.dll`: the host does not load it, and with none in the root the game stops with exit 78.
- Install into another account's root, or into the root of an elevated administrator that is not the player: pass `-Root` to the
  script, or write to the player's own `%LOCALAPPDATA%`.
- Replace `BugSplat64.dll` without keeping the game's own file: it is the only way back to an unmodified install.
- Edit or delete the files the runtime writes in the root while the game runs.

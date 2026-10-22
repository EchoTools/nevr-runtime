# ADR 0012: the install folder is static; nEVR's runtime, plugins and state live in `%LOCALAPPDATA%\nevr\echovr`

Status: proposed (#575, tracking issue #799). Nothing in the "Decision" section is built yet. ADR 0010 and ADR 0011 are accepted and
neither is superseded by this record. What exists today is named under
"Context"; each piece that moves is re-measured by the pull request that moves it.

## Context

The game's install folder holds things that change and things that are secret, next to files that never change:

- the runtime is `BugSplat64.dll` itself, one DLL that carries the whole of `src/runtime/` (ADR 0008 in `0008-bugsplat-host-and-nevr-runtime.md`, "The tree today"), so every
  runtime fix replaces the file the game imports;
- plugins load from a `plugins/` directory next to the game binary (`src/runtime/ext/plugin_loader.cpp`);
- `config.yaml` is found in `_local` next to the game module and one and two parents up, or beside the `-config`/`-config-path` file
  (`FindNevrConfigYamlPath` in `src/runtime/lifecycle/service_config.cpp`);
- the sign-in cache `.credentials.json` is chosen among the same `_local` directories (`SelectCredentialCacheLocation` in
  `src/core/auth_token.h`);
- the log and crash record are written under `%LOCALAPPDATA%\EchoVR\logs`, the original game's own folder
  (`src/runtime/log/builtin_filter.cpp`, `src/runtime/lifecycle/crash_recovery.cpp`).

Three consequences follow. A shared zip of an install carries the owner's secrets with it (#574). Two roles on one install collide:
a dedicated server picked up a player's token (#512, fixed for the server by #516 with a cache beside the server's own
`config.yaml`). And updating the runtime or a plugin needs write access to the install folder, which is `Program Files` or a Steam
library for many players.

The host side already exists in the tree and is not part of the shipped DLL yet: `src/runtime/host/` holds the diagnostics, the
minidump path, the runtime loader (`nevr_host::ResolveRuntimePath`) and the rendezvous on the game's main wrapper (ADR 0008). The
host already writes its own files under `%LOCALAPPDATA%\nevr\echovr\{logs,crashes}`.

## Decision

### The root

One root per game: `%LOCALAPPDATA%\nevr\echovr\`. The install folder keeps the game's files and `BugSplat64.dll`, the stable host
(ADR 0008), and nothing else of ours. The root holds:

| In the root | What it is |
| --- | --- |
| `nevr.dll` | the changing runtime |
| `plugins\` | plugin DLLs and their dependencies |
| `config.yaml` | the nEVR configuration, when no `-config` folder is given |
| `.credentials.json` | the sign-in cache for the config folder it sits in |
| caches, `logs\`, `crashes\` | what the runtime and the host write |

`%LOCALAPPDATA%\EchoVR` stays the original game's folder; nothing of ours is written there.

Updating `nevr.dll` or a plugin writes only the root, so it needs no administrator rights, and the install can stay in
`Program Files` or a Steam library untouched. A clean install is the game's files plus `BugSplat64.dll` in the install folder, and
the installer puts `nevr.dll` in the root.

### What the host keeps

The rule for what lives in `BugSplat64.dll`: only what must keep working when `nevr.dll` does not, and that rarely changes,
because the file sits in the install folder.

1. **Crash capture stays in the host.** A crash inside `nevr.dll` is still recorded (the host's `createReport`, ADR 0008).
2. **The first log line is versions:** the host's version and sha256, then `nevr.dll`'s version and sha256, so a player's report
   names exactly what ran.
3. **A refused or missing `nevr.dll` stops the game**, after one refusal dialog and its log line. The game does not continue
   without nEVR, and there is no fallback to a single-DLL install.

A last-known-good copy of `nevr.dll` is out of scope until there is a reason for one. The plugin loader stays in `nevr.dll`.

### The host verifies `nevr.dll` before it loads it

A root a player can write is a way in, so the host verifies the file before it maps it, with the verifier ADR 0010 defined for
plugins: the source files `src/runtime/ext/plugin_signature.cpp` and `src/runtime/ext/plugin_trust.cpp` compile into the host and
into `nevr.dll` unchanged, so there is one implementation and one set of pins. The file is opened `FILE_SHARE_READ` and held open
across the load, as the plugin loader does.

The host's policy is the production policy of ADR 0010 without the denylist: the pinned Microsoft root and the profile EKU. The
denylist stays in `nevr.dll`, which applies it to plugins and, once loaded, to itself. A denylist change must not need a new
`BugSplat64.dll`, because that file changes rarely.

The dialog text for a refused file names the file and the reason in a player's words. For a missing `nevr.dll` it says that
`nevr.dll` is missing and to run the installer, and the log line names the path that was searched. Both go through the refusal
policy of ADR 0010 (`src/runtime/ext/plugin_refusal_notice.h`): one dialog per run, and none on a dedicated server or a headless
run, which get the log line and a non-zero exit instead.

A build that allows unsigned plugins (the `NEVR_ALLOW_UNSIGNED_PLUGINS` build of ADR 0010) allows an unsigned `nevr.dll` too, and
carries the same literal. A signed-only build refuses it.

### Where state resolves

- `nevr.dll` and `plugins\` resolve under the root. The plugin directory filter of ADR 0010 follows the plugin directory.
- `config.yaml` is the one in the folder of the `-config` or `-config-path` file when one is given, and otherwise the root's.
- `.credentials.json` sits beside the `config.yaml` the process was launched with: the folder of the `-config` file, and without
  one the root. This is the rule #516 set for a dedicated server, on the new layout; there is no further folder scheme. A server
  and a player on one machine share nothing unless they share a config folder.
- caches, logs and the crash record resolve under the root; the host's own paths are already there.

A build that allows unsigned plugins also honours one environment variable, `NEVR_ROOT`, which replaces `%LOCALAPPDATA%\nevr\echovr`
for harnesses and native test machines. A signed-only build ignores it and logs that it did. Test deployments under Wine put
`nevr.dll` and the plugins into the Wine prefix's own root, so a run on a game copy never touches the user's.

### The one-time move

On the first run that finds the old layout, the runtime moves, and logs every file it moves or leaves:

- from the `_local` directories (and the `-config` folder's neighbours the old search used): `config.yaml`, `.credentials.json`
  and the caches;
- from `%LOCALAPPDATA%\EchoVR\logs`: nEVR's own files, which are the run logs the built-in filter writes (`BuildLogFilePath` in
  `builtin_filter.cpp`, whose prefix is configurable) and `nevr-crash-<run id>.txt` (`PrepareCrashRecordPath`), never the game's own.
  The move recognises them by the names those writers use; PR 5 fixes the list from the code of that day.

The credentials move first, so a player who updates keeps their login. The move never overwrites a file already in the root, is
safe to run twice, and leaves a file in place, with a log line, when it cannot move it. Nothing is deleted that was not moved.

### Updating only the host

A player who replaces `BugSplat64.dll` alone, from an old zip or by hand, lands on a missing `nevr.dll`: the game stops with the
dialog above. That is the intended outcome; the installer is the supported way to update.

## Relation to other records

- ADR 0008 is built on, not changed: the host and runtime are a matched pair behind one versioned C ABI, and the rendezvous is the
  game's main wrapper. This record decides where the runtime loads from and what the host checks first.
- ADR 0010 is extended, not superseded: its verifier gains a second use, the host's check of `nevr.dll`, and its refusal policy
  gains a second subject.
- ADR 0011 (Quest game data) concerns the headset and is unaffected.

## Consequences

- The install folder is the same for every release of the runtime; an update is a file write in the user's folder.
- The shipped `BugSplat64.dll` carries the verifier, so a change to the pinned root or the profile EKU is a change to the host,
  which is deliberate: those pins are the trust anchor.
- A refused or missing `nevr.dll` is a stopped game, not a degraded one. A host that cannot find a signed runtime does not run
  unmodified code against a service that expects nEVR.
- Packaging, the installer and uninstaller, the launch and verify scripts and the Wine and native test deployments all change to
  install `nevr.dll` and the plugins into the root; a package that carries the host without the runtime is a defect.

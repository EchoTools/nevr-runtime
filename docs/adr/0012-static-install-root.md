# ADR 0012: the install folder is static; nEVR's runtime, plugins and state live in `%LOCALAPPDATA%\nevr\echovr`

Status: proposed (#575, tracking issue #799). Nothing in the "Decision" section is built yet. ADR 0010 and ADR 0011 are accepted and
neither is superseded by this record. **It amends ADR 0008 (`0008-bugsplat-host-and-nevr-runtime.md`) in three places:** `nevr-runtime.dll`
loads from the root, not from the directory of `BugSplat64.dll`; a refused or missing `nevr-runtime.dll` stops the game, where ADR 0008
lets it run without the runtime; and the host's log gains a `versions` event. "Relation to other records" says which text of ADR 0008 each replaces. What exists today is named
under "Context"; each piece that moves is re-measured by the pull request that moves it.

The runtime DLL is named `nevr-runtime.dll`, in this record, in ADR 0008, in `src/runtime/host/`, in its tests and in `tools/verify_pe_contract.py`.

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
a dedicated server picked up a player's token (#512). Servers and clients sign in one way, by token, from one cache
(`SelectCredentialCacheLocation` in `src/core/auth_token.h` takes only the executable's directory). And updating the runtime or a plugin needs write access to the install folder, which is `Program Files` or a Steam
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
| `nevr-runtime.dll` | the changing runtime |
| `plugins\` | plugin DLLs and their dependencies |
| `config.yaml` | the nEVR configuration, when no `-config` folder is given |
| `.credentials.json` | the sign-in cache, one per machine, for clients and servers alike |
| caches, `logs\`, `crashes\` | what the runtime and the host write |

`%LOCALAPPDATA%\EchoVR` stays the original game's folder; nothing of ours is written there.

These names and this layout are fixed by this record (see "Names this record fixes"). Where a file's own name is not in the table
(the caches), the pull request that moves it names it and this record is amended.

Updating `nevr-runtime.dll` or a plugin writes only the root, so it needs no administrator rights, and the install can stay in
`Program Files` or a Steam library untouched. A clean install is the game's files plus `BugSplat64.dll` in the install folder, and
the installer puts `nevr-runtime.dll` in the root.

### What the host keeps

The rule for what lives in `BugSplat64.dll`: only what must keep working when `nevr-runtime.dll` does not, and that rarely changes,
because the file sits in the install folder.

1. **Crash capture stays in the host.** A crash inside `nevr-runtime.dll` is still recorded (the host's `createReport`, ADR 0008).
2. **The versions are logged as soon as the verdict on `nevr-runtime.dll` is known**, in the `versions` record of "Names this record fixes":
   the host's version and sha256, then `nevr-runtime.dll`'s version and sha256, so a player's report names exactly what ran.
3. **A refused or missing `nevr-runtime.dll` stops the game**, after one refusal dialog and its log line. The game does not continue
   without nEVR, and there is no fallback to a single-DLL install. The load, the check, the dialog and the stop run in the host's
   detour on the game's main wrapper (ADR 0008's rendezvous), never in `DllMain`: ADR 0008's rule that `DllMain` does no file I/O
   and returns `TRUE` stands, and the dialog is shown without the loader lock held.
4. **The host's export table does not change** (the names and ordinals `tools/verify_pe_contract.py` pins). Logging stays in
   the host as ADR 0008 describes it (`nevr-bootstrap.jsonl`, `crash_capture`).

A last-known-good copy of `nevr-runtime.dll` is out of scope until there is a reason for one. The plugin loader stays in `nevr-runtime.dll`.

### The host verifies `nevr-runtime.dll` before it loads it

A root a player can write is a way in, so the host verifies the file before it maps it, with the verifier ADR 0010 defined for
plugins: the source files `src/runtime/ext/plugin_signature.cpp` and `src/runtime/ext/plugin_trust.cpp` compile into the host and
into `nevr-runtime.dll` unchanged, so there is one implementation and one set of pins. The file is opened `FILE_SHARE_READ` and held open
across the load, as the plugin loader does.

The host's policy is the production policy of ADR 0010 without the denylist: the pinned Microsoft root, the profile EKU and the
code-signing EKU, as ADR 0010 requires. Today `ProductionPolicy` and `plugin_trust.cpp` are runtime units that also carry the
`NEVR-PLUGIN-TRUST` literal; the host takes the same pins through a second policy constructor over the same constants, not a
copy, and the pull request that does it measures what moves. The denylist stays in `nevr-runtime.dll` and covers plugins. A denylist change
must not need a new `BugSplat64.dll`, because that file changes rarely. The accepted cost: a genuinely signed `nevr-runtime.dll` that is on
the denylist is stopped only by a new host release; the host changes rarely, and that is the price of keeping it small.

The dialog text for a refused file names the file and the reason in a player's words. For a missing `nevr-runtime.dll` it says that
`nevr-runtime.dll` is missing and to run the installer, and the log line names the path that was searched. Both go through the refusal
policy of ADR 0010 (`src/runtime/ext/plugin_refusal_notice.h`): one dialog per run, and none on a dedicated server or a headless
run, which get the log line and a non-zero exit instead.

A dev build (the `NEVR_ALLOW_UNSIGNED_PLUGINS` build of ADR 0010) loads an unsigned `nevr-runtime.dll`, with the trust-mode log line ADR 0010
defines, and carries the same `NEVR-PLUGIN-TRUST` literal; a plugin developer's dev host must load a dev `nevr-runtime.dll`. A signed-only
build refuses it.

### Where state resolves

- `nevr-runtime.dll` and `plugins\` resolve under the root. The plugin directory filter of ADR 0010 follows the plugin directory.
- `config.yaml` is the one in the folder of the `-config` or `-config-path` file when one is given, and otherwise the root's.
- **Clients and servers:** one `.credentials.json` at the root (#574's decision), the sign-in cache of the account on that machine;
  a client or a server launched with a `-config` elsewhere keeps its login at the root as well. A dedicated server signs in the way a
  client does, by token, from the same cache (`SelectCredentialCacheLocation` takes only the executable's directory); there is no server-only cache
  beside the server's own `config.yaml`. A server and a player on one machine therefore share the cache; where the file sits does not prevent
  the collision of #512, and whether something else must is not decided here.
- #574 is folded into this record: the root's `.credentials.json` is its decided path.
- caches, logs and the crash record resolve under the root; the host's own paths are already there.

A build that allows unsigned plugins also honours one environment variable, `NEVR_ROOT`, which replaces `%LOCALAPPDATA%\nevr\echovr`
for harnesses and native test machines. A signed-only build ignores it and logs that it did. Test deployments under Wine put
`nevr-runtime.dll` and the plugins into the Wine prefix's own root, so a run on a game copy never touches the user's.

### The one-time move

On the first run that finds the old layout, the runtime moves, and logs every file it moves or leaves:

- from the install's `_local` directories: `config.yaml`, `.credentials.json` and the caches. A `-config` folder's own
  `config.yaml` is not moved: it stays where it is, and the rule of "Where state resolves" finds it there;
- from `%LOCALAPPDATA%\EchoVR\logs`: nEVR's own files, which are the run logs the built-in filter writes (`BuildLogFilePath` in
  `builtin_filter.cpp`, whose prefix is configurable) and `nevr-crash-<run id>.txt` (`PrepareCrashRecordPath`), never the game's own.
  The move recognises them by the names those writers use; step 5 of the order of work fixes the list from the code of that day.

The credentials move first, so a player who updates keeps their login. The move never overwrites a file already in the root, is
safe to run twice, and leaves a file in place, with a log line, when it cannot move it. Nothing is deleted that was not moved. When
it has moved anything out of a `_local` directory, it writes `MOVED-TO-NEVR-ROOT.txt` there, naming the root, so a player or support
can see why that folder is empty.

The uninstaller puts back what the move took: it copies `config.yaml` and `.credentials.json` from the root into `_local` and removes
`MOVED-TO-NEVR-ROOT.txt`, and leaves the root as it is, so uninstalling and then installing a pre-split release keeps the login.

### Updating only the host, and going back

A post-split `BugSplat64.dll` whose `nevr-runtime.dll` is absent or refused stops the game with the dialog above. That case arises when
the host is replaced or the root is cleared; the installer is the supported way to update.

A `BugSplat64.dll` from a pre-split release is the whole runtime in one DLL (ADR 0008, "The tree today"): it never looks for
`nevr-runtime.dll`, so it never shows that dialog. Dropped over a migrated install it runs the old runtime against `_local` paths the move
emptied: no config and no login. Going back to a pre-split release therefore requires the uninstaller first, which restores `_local` (see "The one-time move"), and the
marker file is what shows a player or support why the old folder is empty.

## Order of work

Each step is its own pull request with its own issue under #575, mergeable and testable alone:

0. This record (ADR 0010 and 0011 are already on main).
1. **1a, the flip:** the runtime's initialization leaves `DllMain` for the wrapper detour, with the sensors and tests that pin the
   old shape. **1b, the host minidump:** the host's `BugSplat_createReport` writes the minidump.
2. The target split: `nevr-runtime.dll` is built beside `BugSplat64.dll`, in the same folder, and the package is a matched pair.
3. The host verifies `nevr-runtime.dll` (the signature gate with the pins above), writes the `versions` record, and shows the dialog and stops
   on a refusal or a missing file. **Prerequisite:** `release-signer` signs `nevr-runtime.dll` (it is added to the signer's file list); this
   step does not ship until a signed `nevr-runtime.dll` exists.
4. `nevr-runtime.dll` and the plugins move to the root, with the installer and uninstaller (which restores `_local`) and the dev-only
   `NEVR_ROOT`.
5. State moves under the root, with the one-time move (#574). Clients and servers both resolve `.credentials.json` there.

## Names this record fixes

- **The runtime DLL's name:** `nevr-runtime.dll`, in the root, signed by the release signer.
- **The root layout and file names:** `%LOCALAPPDATA%\nevr\echovr\` holding `nevr-runtime.dll`, `plugins\`, `config.yaml`,
  `.credentials.json`, `logs\` and `crashes\`, as in the table. `nevr-bootstrap.jsonl` stays the host's log file name.
- **`NEVR_ROOT`**, honoured only in a dev build, replaces `%LOCALAPPDATA%\nevr\echovr`; it lands in step 4.
- **The `versions` record:** written by the host's detour on the game's main wrapper right after the verdict on `nevr-runtime.dll`: the first
  record of that process after the verdict (attach records before it are fine; every record carries the `pid`, so the log reads per
  process). It goes to `nevr-bootstrap.jsonl` with `tag` `NEVR.HOST`, `event` `versions`, and the fields `host_version`, `host_sha256`, `runtime_version`, `runtime_sha256` (the last two
  `unavailable` when `nevr-runtime.dll` is missing or refused, with the path searched in `runtime_path`).
- **The exit code** after the refusal dialog on a client, and on a server or headless run, is 78 (free in `src/` today; 78 is `EX_CONFIG`
  in sysexits). The host ends the process by its own call, `TerminateProcess(GetCurrentProcess(), 78)`, as it already does for an
  unsupported game image (`RequireSupportedGameModule`, `src/runtime/lifecycle/game_image_guard.cpp`, called from
  `src/runtime/host/host_entry.cpp`), never through the runtime's `ForceFatalExit`, which belongs to a runtime that is not there.
  A server also logs the refusal line first.
- **The host's export table** is unchanged.

## Relation to other records

- ADR 0008 is built on and **amended in three places**. Kept: the host and runtime are a matched pair behind one versioned C ABI, the
  rendezvous is the game's main wrapper, `DllMain` does no file I/O, and the export table. Replaced: "`nevr-runtime.dll`, in the same
  directory" and the `ResolveRuntimePath` rule "`nevr-runtime.dll` in the directory of `BugSplat64.dll`" become the root; and "when no runtime is published the
  detour returns the original wrapper's result" becomes a stop; and the host's journal events (`host_attach`, `runtime_load`, `crash_dump`) gain a `versions` event. The code that implements the first is
  `nevr_host::ResolveRuntimePath` (`src/runtime/host/runtime_load.cpp`).
- ADR 0010 is extended, not superseded: its verifier gains a second use, the host's check of `nevr-runtime.dll`, and its refusal policy
  gains a second subject.
- ADR 0011 (Quest game data) concerns the headset and is unaffected.

## Consequences

- The install folder is the same for every release of the runtime; an update is a file write in the user's folder.
- The shipped `BugSplat64.dll` carries the verifier, so a change to the pinned root or the profile EKU is a change to the host,
  which is deliberate: those pins are the trust anchor.
- A refused or missing `nevr-runtime.dll` is a stopped game, not a degraded one. A host that cannot find a signed runtime does not run
  unmodified code against a service that expects nEVR.
- Packaging, the installer and uninstaller, the launch and verify scripts and the Wine and native test deployments all change to
  install `nevr-runtime.dll` and the plugins into the root; a package that carries the host without the runtime is a defect.

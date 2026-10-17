# ADR 0008: BugSplat64.dll is the stable host, nevr.dll is the runtime

Status: accepted. The contract, the descriptor, the host units and their harnesses are
in the tree, and the rendezvous is decided ("Decision: the rendezvous is the game's main
wrapper"). `BugSplat64.dll` is still built from the whole of `src/runtime/` as one DLL:
the host units are compiled only into the test harnesses until the target split.

## Context

The game statically imports `BugSplat64.dll`, so whatever carries that name loads
before `WinMain` and is the file every install and every test deployment replaces.
The runtime changes with every release; the part the game needs from that name
(nine `MiniDmpSender` methods, a place to write a minidump, a place to load the
runtime from) does not. Keeping both in one DLL means every runtime change replaces
the stable file, and a runtime that fails to load takes the crash reporter with it.

## Decision

`BugSplat64.dll` is the stable host. `nevr.dll`, in the same directory, is the changing
runtime. The two speak one versioned C ABI and nothing else crosses between them.

### The host's PE contract

`tools/verify_pe_contract.py` pins the export table of `BugSplat64.dll`: the 26 names
and their ordinals, `DetoursExportPlaceholder` at ordinal 1, and no import of
`nevr.dll`. With the game binary present it also checks that the nine
`BugSplat64.dll` imports of `echovr.exe` are imported by name, are the pinned set and
are exported. `just verify` runs it on the built DLL. A PE export table carries no
types, so signatures and calling conventions are not covered.

Export ownership, from the callers in the repository, `plugins/` and `src/extension/`:

| Export | Caller | Owner |
| --- | --- | --- |
| nine `MiniDmpSender` methods | `echovr.exe` | host; `createReport` writes the minidump |
| `DetoursExportPlaceholder` | suspended-process loaders | host |
| `MiniDumpWriteDump` | none in the game binary; compatibility export | host |
| `NEVR_SetGameModule` | an external launcher | host |
| `NEVR_RegisterResourceOverride`, `NEVR_ResetResourceOverrides`, `NEVR_DeregisterResourceOverrides` | plugins, through `plugins/common/include/resource_registry.h` | runtime; the host forwards |
| `NEVR_GetUPnPConfig`, `NEVR_ScheduleReturnToLobby` | no caller in the repository | runtime; the host forwards |
| `platform_compat_*`, `token_auth_*`, `TokenAuth_*` | `src/runtime/lifecycle/boot.cpp` and `test_token_auth` inside one image | runtime |

### The runtime descriptor

`src/core/runtime_abi.h` defines it. `nevr.dll` exports one function,
`NEVR_GetRuntimeApi`, that returns a `NevrRuntimeApi` (`initialize`, `game_main`). The
host validates the major version, the minimum `struct_size` and every required entry
before it calls anything, and hands the runtime a `NevrHostContext` that carries the game
module, the trampoline of the original `GameMainWrapper`, and a record of the host's hook
on it: the target address and the first 16 bytes found there after the host enabled the
hook. Only C types cross. `test_runtime_abi` pins the
layout and the rejections, including a descriptor shorter than the header, which the
validator must not read past.

### Early start without the runtime under the loader lock

The host's attach path, in direct-import mode: `DllMain` does three things: validate the
game image (`nevr_game_image_guard`), validate the pinned 13-byte prologue of
`GameMainWrapper` (RVA `0xCD510`) and install one MinHook detour on it. It loads nothing
and does no file I/O. The runtime loads on the first call of the detour, after attach has
returned, and the detour returns `game_main(command_line)`; without a runtime it returns
the original wrapper's result. Every failure leaves the game unhooked or running the
original and returns `TRUE`, because the game imports the DLL statically and a `FALSE`
aborts process start.

`src/runtime/tests/bootstrap_attach` is the proof gate for that path. It is built from the
production host units (`src/runtime/host/`) behind a `BugSplat64.dll` shim, so it measures
the code the game runs. The shim attaches inside child processes against a synthetic image
that starts with the real prologue bytes, and a static-import executable attaches it
before `main` and enters the wrapper from `main`. The scenarios cover a missing,
incompatible, entry-less and failing-init runtime, concurrent first calls, threads calling
the target across the attach, `DllMain` held open with a live hook, same-thread re-entry
during initialization, a wrong timestamp and a wrong prologue. Every child checks that the
host recorded one `hook_install` when it hooked and none when it did not, and that the
runtime never hooks the wrapper: the first 16 bytes read after the runtime's `initialize`
equal the host's record of its hook. What it measured:

- a same-thread re-entry during runtime load must bypass to the original;
  `InitOnceExecuteOnce` recursion on the loading thread deadlocks;
- the trampoline and the host context must be published before `MH_EnableHook`,
  because a racing thread enters the detour before `MH_EnableHook` returns;
- the load waits for the hook install to be journalled before it reports the journal,
  for the same reason: the install record is written after `MH_EnableHook` returns;
- an attach failure returns `TRUE` and leaves the game unhooked.

The harness runs under Wine in `just verify`. It has not run on native Windows.

The production path is `src/runtime/host/host_entry.cpp`, which the host's `DllMain`
and its exports call (`OnProcessAttach`, `SetGameModule`, `CreateReport`,
`OnCrashReporterConstructed`), and `runtime_rendezvous.cpp` (the detour,
`InitOnce`-guarded load, the dispatch). `src/runtime/tests/rendezvous` drives those
objects behind a `BugSplat64.dll` shim: a valid runtime, a missing runtime, same-thread
re-entry from `initialize` and a wrong prologue, then `NEVR_SetGameModule` with a
supported and an unsupported module. Each child asserts the ordered `host_attach` and
`runtime_load` records; for a valid runtime that `initialize` ran once and `game_main`
once, with the exact command line and the exact result going back to the caller; and for a
missing runtime that the original wrapper ran once. The runtime in that harness is a
stand-in whose `game_main` runs the original wrapper through the trampoline; it is not
`nevr.dll`.

### The host's own files

The host writes under its own per-game folder, never the game's `EchoVR` folder:

- `%LOCALAPPDATA%\nevr\echovr\logs\nevr-bootstrap.jsonl`, then
  `%TEMP%\nevr\echovr\logs`, then the directory of `BugSplat64.dll`;
- `%LOCALAPPDATA%\nevr\echovr\crashes\nevr-<pid>-<n>.dmp`, then
  `%TEMP%\nevr\echovr\crashes`, then the directory of `BugSplat64.dll`.

Every record is one JSON object per line with `tag` (`NEVR.HOST`), `ts` (UTC),
`pid` and `event`. The events are `host_attach` (one per decision: `image_check`,
`prologue_check`, `hook_install`; the last two carry the target's `rva` and expected and
actual prologue, and `hook_install` the MinHook `stage` and `mh_status`, and
`hook_bytes: unread` when the hook is live but its first bytes could not be read;
`install_wait`: `signalled`, or `unavailable` / `timeout` with what the load does next;
`detour_entry`, a defect; `set_game_module`, a second call refused; `journal`, how many
decisions a full journal dropped), `runtime_load`
(outcome, path, Win32 error, expected and actual ABI, ABI status, init code, degraded
action, and `loader_lock_held`, which the host reads from the PEB) and `crash_dump`
(written, or each destination that failed and why, suppressed re-entry, dbghelp
unavailable, failed). A line that fits no directory goes to `stderr`; a record is never
dropped silently, and one too large for the 4 KiB bound is replaced by a marker, not
truncated.

Each append to the file holds a named mutex (`Local\nevr-host-bootstrap-log`) for the write
only. Under Wine a `FILE_APPEND_DATA` write is not atomic, and concurrent appenders overwrote
each other's lines, within one process and across two that share a prefix (a client and a
local server). The mutex covers both. The wait is bounded (500 ms) and the mutex is recursive
for its owner, so a crash inside an append, or a holder that never releases, delays the crash
record by at most the wait and never blocks it: it is then written without the lock.

Two builders produce those lines. `nlohmann::json` builds the records written after
attach has returned. `nevr_host::JsonLine` (`src/runtime/host/host_json.h`) builds them
in a caller-owned fixed buffer for the crash callback, which cannot allocate; every
line it writes is parsed back in `test_host_json` and in the crash harness. The attach
decisions are stored in a fixed journal while the loader lock is held and written once
the lock is released.

### The minidump path

`BugSplat_createReport` (the `MiniDmpSender::createReport` export) writes a minidump
with the `MiniDumpWriteDump` of the already-loaded `dbghelp.dll`, resolved with
`GetModuleHandleW`; the game's `HandleCrashDump` loads dbghelp before it calls the
reporter, so the host never loads it. It tries the three crash directories in order,
holds an atomic re-entry guard that is cleared on return, and falls back to a
fixed-size text file when no dump can be written. No logger, lock, heap or runtime
load is involved. `src/runtime/tests/host_crash` faults a child process through the
real mangled export, with `nevr.dll` absent, ABI-incompatible, entry-less,
failing in `initialize` and compatible, with each destination blocked in turn, and
validates the dump header, the faulting thread and process, and every record.

### Where the runtime loads from

`nevr_host::ResolveRuntimePath` (`src/runtime/host/runtime_load.cpp`) is the one
function that decides which file is the runtime: `nevr.dll` in the directory of
`BugSplat64.dll`, loaded with `LoadLibraryExW` and the DLL-load-directory and
System32 search flags. A failed runtime stays mapped: it may already have patched
something, and unmapping live code is worse than a leaked module.

## Decision: the rendezvous is the game's main wrapper

The host installs one detour from its `DllMain`, on the game's main wrapper at
`echovr.exe` `0x1400CD510` (`GAME_MAIN_WRAPPER` in `src/runtime/hook/addresses.h`). The
game calls that wrapper once, on the main thread, with the command-line pointer. The
wrapper calls `0x1400CD550` (`GameMain`), which calls the run function in vtable slot
`0x3F0` (`0x14050B6E0`), whose first call is vtable slot 0, `PreprocessCommandLine`.

After the split, at the wrapper's entry the detour loads the runtime and calls its
`initialize`, then returns `game_main(command_line)`. `game_main` runs `GameMain` under the
runtime's `setjmp`, which the access-violation handler `longjmp`s back to (N125). The
recovery arms because the `setjmp` frame is the frame that calls `GameMain`: the host's
detour calls `game_main` from the wrapper's entry, so that frame is live for the whole run
and the `longjmp` returns into it. A detour installed later, by the runtime's `initialize`
at the `PreprocessCommandLine` rendezvous, would never be entered, because the wrapper is
already on the stack by then. The `setjmp` and the `longjmp` stay in
`src/runtime/lifecycle/crash_recovery.cpp` beside each other. Everything the runtime hooks
installs at the wrapper's entry, before `GameMain` reaches `PreprocessCommandLine`.
`BuildCmdLineSyntaxDefinitions` is called from inside `PreprocessCommandLine`
(`0x140503ADF`, in the first callee of `PreprocessCommandLine`), so it is installed in time
as well. When no runtime is published the detour returns the original wrapper's result.

The rule for library loads, and nothing more: no runtime hook acts on a library load before
the wrapper's entry. The runtime's load filters, post-load observers and DLL-load
callbacks apply from the moment the runtime registers them.

What the rendezvous leaves outside the runtime:

- Game `CLog::PrintfImpl` calls made before the wrapper's entry reach the game's log
  without the runtime's filter.
- On native Windows the runtime's vectored handler and console handler register at the
  wrapper's entry. A fault before it is handled by the game's filter and the host's
  minidump. N82 says only that no handler other than the one in `crash_recovery.cpp` acts
  on `EXCEPTION_BREAKPOINT`; other vectored handlers exist (`plugins/common/include/safe_memory.h`
  adds one) and return `EXCEPTION_CONTINUE_SEARCH` for it. The host registers none.

### The tree today

The shipped `BugSplat64.dll` is still the whole of `src/runtime/` in one DLL, and it hooks
the wrapper itself: `InstallGameMainHook` in `crash_recovery.cpp`, called from
`initialize.cpp` in `DllMain`, detours the wrapper with `PatchDetour`, and
`GameMainWrapperHook` holds the `setjmp`. The runtime has no `game_main` entry yet. The
descriptor, the host units and their harnesses describe the target. The flip comes first: the
runtime adopts the host contract inside the one DLL, so `initialize` runs from the host's
detour at the wrapper's entry, `game_main` replaces `GameMainWrapperHook`, and the runtime
stops hooking the wrapper. The split follows: the runtime moves into `nevr.dll`. Until the
flip the harness runtimes are stand-ins whose `game_main` runs the original wrapper through
the trampoline.

Vocabulary: "host" in this record is `BugSplat64.dll`. In the plugin ABI
(`src/extension/plugin_interface.h`) "host" is the runtime.

## Consequences

- `BugSplat64.dll` and `nevr.dll` are a matched pair; a package or a test deployment
  that carries one without the other is a defect, not a configuration.
- The export table is part of the contract: a change to it is a change to
  `tools/verify_pe_contract.py` in the same commit.
- Until the target split, `src/runtime/host/` (the entry points, the rendezvous, the
  diagnostics, the crash capture and the loader) is compiled only into the test
  harnesses. The shipped `BugSplat64.dll` does not use it.
- After the flip the runtime does not hook the main wrapper; the host's detour is the only
  hook on it. The host context carries that hook's target and the first bytes found there
  after the host enabled it, with a flag (`wrapper_hook_bytes_valid`) that says whether those
  bytes were read. A context whose bytes were not read is accepted, because refusing to start
  over a HookGuard registration would take every runtime fix with it; the runtime registers
  the hook only when `HookRecordUsable` says the record is usable. When it says no, the
  runtime logs a Warning in its own log, registers nothing for the wrapper (HookGuard then
  cannot verify that hook, and the log says so) and keeps running; it does not register
  zeros. `HookRecordUsable` does not re-check the header, the target or the flag's range, so
  it is called only on a context `ValidateHostContext` accepted. The host module is required.
  The shipped DLL does none of this yet ("The tree today").
- The host's `DllMain` validates the image and the wrapper's prologue, installs the one
  detour and journals each decision. It loads nothing and does no file I/O.

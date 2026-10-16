# ADR 0008: BugSplat64.dll is the stable host, nevr.dll is the runtime

Status: accepted. The contract, the descriptor, the host units and their harnesses
are in the tree. `BugSplat64.dll` is still built from the whole of `src/runtime/` as
one DLL: the target split waits on the decision in "Open decision" below.

## Context

The game statically imports `BugSplat64.dll`, so whatever carries that name loads
before `WinMain` and is the file every install and every test deployment replaces.
The NEVR runtime changes with every release; the part the game needs from that name
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
`NEVR_GetRuntimeApi`, that returns a `NevrRuntimeApi` (`initialize`,
`preprocess_before`, `preprocess_after`). The host validates the major version, the
minimum `struct_size` and every required entry before it calls anything, and hands
the runtime a `NevrHostContext` that carries the game module and the trampoline of
the original `PreprocessCommandLine`. Only C types cross. `test_runtime_abi` pins the
layout and the rejections, including a descriptor shorter than the header, which the
validator must not read past.

### Early start without the runtime under the loader lock

The host's attach path, as the proof gate measures it, in direct-import mode: `DllMain` does three things: validate the game image
(`nevr_game_image_guard`), validate the pinned 18-byte prologue of
`PreprocessCommandLine` (RVA `0x116720`) and install one MinHook detour on it. It
loads nothing and does no file I/O. The runtime loads on the first call of the
detour, after attach has returned, and the detour dispatches
`preprocess_before` -> original, once -> `preprocess_after`. Every failure leaves the
game unhooked or running the original and returns `TRUE`, because the game imports the
DLL statically and a `FALSE` aborts process start.

`src/runtime/tests/bootstrap_attach` is the proof gate for that path. A probe host
built as `BugSplat64.dll` attaches inside child processes against a synthetic image
that starts with the real prologue bytes, and a static-import executable attaches it
before `main`. The scenarios cover a missing, incompatible, entry-less and
failing-init runtime, concurrent first calls, a thread calling the target across the
attach, `DllMain` held open with a live hook, same-thread re-entry during
initialization, a wrong timestamp and a wrong prologue. What it measured:

- a same-thread re-entry during runtime load must bypass to the original;
  `InitOnceExecuteOnce` recursion on the loading thread deadlocks;
- the trampoline and the host context must be published before `MH_EnableHook`,
  because a racing thread entered the detour before `MH_EnableHook` returned;
- an attach failure returns `TRUE` and leaves the game unhooked.

The harness runs under Wine in `just verify`. It has not run on native Windows.

### The host's own files

The host writes under NEVR's own per-game folder, never the game's `EchoVR` folder:

- `%LOCALAPPDATA%\nevr\echovr\logs\nevr-bootstrap.jsonl`, then
  `%TEMP%\nevr\echovr\logs`, then the directory of `BugSplat64.dll`;
- `%LOCALAPPDATA%\nevr\echovr\crashes\nevr-<pid>-<n>.dmp`, then
  `%TEMP%\nevr\echovr\crashes`, then the directory of `BugSplat64.dll`.

Every record is one JSON object per line with `tag` (`NEVR.HOST`), `ts` (UTC),
`pid` and `event`. The events are `host_attach` (one per decision: `image_check`,
`prologue_check`, `hook_install`), `runtime_load` (outcome, path, Win32 error,
expected and actual ABI, ABI status, init code, degraded action) and `crash_dump`
(written, or each destination that failed and why, suppressed re-entry, dbghelp
unavailable, failed). A line that fits no directory goes to `stderr`; a record is never
dropped silently, and one too large for the 4 KiB bound is replaced by a marker, not
truncated.

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

## Open decision

The runtime cannot move into `nevr.dll` as designed until the owner decides how it
keeps `GameMainWrapperHook`. The runtime's initialization installs a detour on the
game's main wrapper at `echovr.exe` `0x1400CD510` (`InstallGameMainHook` in
`src/runtime/lifecycle/crash_recovery.cpp`, `GAME_MAIN_WRAPPER` in
`src/runtime/hook/addresses.h`). That detour holds the `setjmp` that the access-violation
handler `longjmp`s back to (N125). The game calls its main wrapper (`0x1400CD510`),
which calls `0x1400CD550`, which calls the run function in vtable slot `0x3F0`
(`0x14050B6E0`), whose first call is vtable slot 0, `PreprocessCommandLine`. By the
time the rendezvous fires, the wrapper is already on the stack, so a detour installed
by the runtime's `initialize` is never entered and the recovery never arms. The other
hooks were checked against the same call order: `BuildCmdLineSyntaxDefinitions` is
called from inside `PreprocessCommandLine` (`0x140503ADF`, in the first callee of
`PreprocessCommandLine`), so it is not affected.

## Consequences

- `BugSplat64.dll` and `nevr.dll` are a matched pair; a package or a test deployment
  that carries one without the other is a defect, not a configuration.
- The export table is part of the contract: a change to it is a change to
  `tools/verify_pe_contract.py` in the same commit.
- Until the target split, `src/runtime/host/` is compiled only into the test
  harnesses. The shipped `BugSplat64.dll` does not use it.

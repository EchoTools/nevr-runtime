# The crash report

When the game crashes, a readable report is written once, by shared code, on PC and Quest. The core is
`src/core/crash_report.h` (namespace `nevr_crash`); each platform supplies the facts only it knows.

## What a report holds

One text file, one fact per line, readable without a tool:

| Line | Content |
| --- | --- |
| `summary:` | the one logged line (below) |
| `signal:` | signal name and number, `code=` (`si_code` on Quest, the access kind on PC), `fault_addr=` |
| `thread:` | thread name and id |
| `pc:`, `lr:` | `module+offset`, `build_id=`, `symbol=<nearest exported symbol>+offset` (`lr` on Quest only) |
| `registers:` | every general register of the faulting thread |
| `backtrace:` | up to 32 frames, each `module+offset symbol` |
| `log (last 50 lines, oldest first):` | the last 50 lines the runtime logged, from a lock-free ring |

A build id is the GNU build id of the module on Quest (read from its `PT_NOTE`, `src/quest/sentinel/elf_build_id.h`),
and `pe-<TimeDateStamp>-<SizeOfImage>` on PC (`src/runtime/lifecycle/crash_report_pe.h`, which also names the nearest
named export). The PC game exports nothing; the server-mode null-dereference sites in
`src/runtime/lifecycle/crash_recovery_sites.h` name a function when the address is exactly its start.

## The logged line

`crash: <signal> <module>+<offset> <symbol> report=<path>`. On PC it carries the `[NEVR.CRASH]` tag; on Quest it goes to
logcat (tag `NEVR-Sentinel`, fatal priority). When the file could not be written it ends
`report=none path=<path> write_failed=1`.

## Where it goes

Beside the minidump, named `<stem>` + `nevr_crash_upload::kReportSuffix` (`src/runtime/compat/crash_upload.h`), which is what
the uploader looks for. It is written whole and atomically: under `<name>.tmp`, then renamed. A first pass with raw
addresses is published before the symbolized one replaces it, because a symbol lookup can block on a lock the faulting
thread holds (`dladdr` on Quest); a hang in the second pass leaves the first.

- PC: `%LOCALAPPDATA%\nevr\echovr\crashes\nevr-crash-<run id>-<n>`, made at init (`InstallCrashFilterInstrumentation`).
- Quest: beside the breakpad minidump in `nevr-crashes`. A handler that reports before breakpad writes it under a
  provisional stem; the dump callback (`sentinel.cpp`) renames it to the minidump's stem.

## What a handler calls

`nevr_crash::WriteReport(CrashInfo)` with the signal or exception code, fault address, pc, lr, a pointer to its own context
(`ucontext_t*` on Quest, `EXCEPTION_POINTERS*` on PC) and the thread id. The platform registered with
`nevr_crash::SetPlatform` reads the registers and the backtrace out of that context. The path allocates nothing, takes no
lock and calls no logger (N70).

Tests: `src/quest/tests/crash_report_test.cpp` and `src/quest/tests/elf_build_id_test.cpp` (`just test-quest-shared`),
`src/runtime/tests/test_crash_report_pe.cpp` (`test_crash_recovery`).

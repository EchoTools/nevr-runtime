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

## What keeps the handler first

The game installs its own crash handler after ours, and the system calls only the last one installed. The handler that
calls `WriteReport` is therefore kept in place and the game's is run after it (`src/core/crash_first.h` holds the rule both
platforms share: the game's request is remembered, not carried out; ours runs once; then the game's):

- Quest: GOT hooks on libr15's libc `sigaction` and `signal` (`src/quest/sentinel/crash_first_hook.h`) answer the game's
  requests for SIGSEGV, SIGBUS, SIGABRT, SIGILL and SIGFPE. The sentinel owns those five handlers (breakpad is built without
  its own): the handler writes the report with the signal's `ucontext_t`, has breakpad write the minidump, puts the actions
  that were in place before the sentinel armed back, then calls the game's handler as the game asked for it. The constructor
  step is `install_crash_first`; the line `{"event":"crash_first_install",...}` says what was installed and logcat gets
  `crash_first: signal=<n> game_handler_called=<0|1> displaced_requests=<n>` after the game's handler returned. The
  libraries libpnsrad, libpnsradmatchmaking and libpnsovr import `sigaction` and `signal` too and the game loads them after
  the constructor; the dlopen hook (live on every launch) hands each load to `OnModuleLoaded`, which hooks their two slots
  right after the game's dlopen returns (`{"event":"crash_first_late",...}`), and again for a mapping loaded again. A
  library's own ELF constructors run inside dlopen, before that moment.
- PC: `kernel32!SetUnhandledExceptionFilter` is hooked (`src/runtime/lifecycle/crash_first_win.h`); the game's filter is
  remembered and ours stays the top-level filter. On an unhandled exception it writes the report (unless `WriteCrashDump`
  already wrote one) and runs the game's filter. `[NEVR.CRASH] first handler installed` says it is in place and
  `[NEVR.CRASH] first_handler code=... game_filter_called=... displaced_requests=...` goes to the crash record after the
  game's filter returned.

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

Beside the minidump, named `<stem>` + `nevr_crash::kReportSuffix` (`src/core/crash_report.h`), which is what
the sender looks for. It is written whole and atomically: under `<name>.tmp`, then renamed. A first pass with raw
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

## Sent to us

On every start, before the game connects to anything, each unsent report goes to nevr-api (`POST /v3/crash-reports`, no
credential, no session) once. The sender is the lowest layer of each platform, so it works when the runtime crashed, failed
to load or never logs in: on PC the BugSplat64.dll host starts it from the detour on the game's main wrapper, before it
loads `nevr-runtime.dll` (`src/runtime/host/crash_send_host.cpp`); on Quest the sentinel starts it from the constructor step
that arms the crash reporter (`src/quest/integration/crash_send_step.cpp`). Both run it on a thread of their own, so a dead
network never delays the game: each request has a 3 s connect and a 6 s total timeout, the pass stops at the first transport
failure, sends at most three reports and starts no request after 10 s.

One implementation, `src/core/crash_send.h` (header only): it finds the reports, reduces the text to the allowlist, builds the
body, reads the answer and marks the file. Each platform supplies only the POST: PC loads `winhttp.dll` by name on the
sender's thread (`crash_send_winhttp.cpp`; the host's imports are pinned to kernel32, crypt32, advapi32 and the UCRT by
`tools/verify_pe_contract.py` and it links no TLS stack), Quest uses the sentinel's libcurl client (`quest/auth/curl_http.h`,
the Android CA store). Both verify the server certificate, follow no redirect and use no proxy.

**What goes out**, and nothing else (`test_crash_send.cpp` fails on any other key): `schema`, `install_id`, `report_id`,
`build`, `platform` (`pc` or `quest`), `headset` (Quest `ro.product.model`; empty on PC), `signal` (`name`, `number`, `code`,
`fault_address`), `pc` and, on Quest, `lr` (`module`, `offset`, `build_id`, `symbol`, `symbol_offset`), and `backtrace` (at most
32 frames of `module`, `offset`, `symbol`). Never the minidump (its stack memory can hold tokens, chat and names), the
registers, the thread name, the log tail the report ends with, an account id, a user name, a path or an IP. The `install_id`
is 128 random bits made on the first send and kept in `install-id.txt` in the crash folder; it comes from nothing about the
person or the machine. A module that is not a plain file name is sent as `unknown`, and a symbol with a path in it is left out.

**Once.** A 2xx renames the report to `<stem>.report.txt.sent`. A 400, 413 or 422, or a report this parser cannot read, renames
it to `<stem>.report.txt.rejected` and it is never offered again. Anything else (429, 5xx, 404, no connection, a timeout)
leaves it for the next start. A 2xx lost on the way back sends the same `(install_id, report_id)` again; the service counts it
once.

**Log.** PC: a `crash_send` record in `nevr-bootstrap.jsonl` (`tag` `NEVR.HOST`); Quest: a `crash_send` line in the sentinel
log. `state` is `sent`, `rejected`, `deferred`, `unreadable`, `install_id_unavailable`, `off` or `pass` (what was sent and
rejected, and how many are left). A folder with nothing to send writes nothing. The report text and the body are never logged.

**Switch.** `nevr_crash_send::kSendEnabled` in `src/core/crash_send.h` (true) turns the send off for a build: while false, neither
platform starts a thread or writes a record. The host's test harnesses never send (`crash_send_host.cpp` returns under
`NEVR_HOST_TESTING`).

### A crash the runtime does not report

A crash the runtime is not there to report still leaves a report, so the next start sends it:

- **PC.** BugSplat64.dll receives every crash through `createReport`. When the runtime is loaded its crash-first filter has
  already written the report, so the host writes none (one crash, one report). When the runtime is absent, failed to load or
  has not loaded yet, or the crash is in the host itself, `CreateReport` (`src/runtime/host/host_entry.cpp`) writes it first, before
  the minidump, through the same shared writer (`src/runtime/host/crash_report_host.cpp`) and the same PC pieces the runtime
  uses (`src/runtime/lifecycle/crash_report_win_common.h`: exception description, registers, backtrace, file calls). The file
  is `nevr-host-<pid>-<time>-<n>` + the report suffix in the same crash folder. The platform is registered at attach (a file name
  from `%LOCALAPPDATA%`, no file I/O); the module table is a snapshot taken at the detour on the game's main wrapper (the
  game's own modules and the host), so a crash before it reports raw addresses.
- **Quest.** The handlers and the report platform are armed by the first statement of the sentinel's constructor
  (`src/quest/sentinel/entry.cpp`), before the begin record or the UI looper capture, so a crash from there on leaves a report.
  Code that runs before the sentinel's constructor is not covered: no handler exists yet.

Tests: `src/runtime/tests/test_host_crash_report.cpp` (a crash with no runtime leaves a report the sender reads),
`tools/tests/test_crash_send_wiring.py` (where each piece is wired).

Tests: `src/runtime/tests/test_crash_send.cpp` (reports made by the real writer, the allowlist, the send), `src/runtime/tests/test_host_crash_send.cpp`
(the WinHTTP POST against a loopback server).

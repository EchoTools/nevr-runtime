# Windows VM system test

Everything else in this repo is tested under Wine. Wine cannot reproduce
Windows-only failures — issue #13 was "works fine under Wine". This test runs the
built `BugSplat64.dll` on a real Windows guest (libvirt) and judges the boot.

```sh
just build
WINVM_USER=... WINVM_PASS=... just test-winvm                    # GAI, boot, and local Nakama login
WINVM_USER=... WINVM_PASS=... just test-winvm --scenario gai     # getaddrinfo timing only
# --scenario login needs a local Nakama, which this tree does not provide.
```

Exit codes: `0` pass, `1` the runtime failed a check, `2` the environment is
unusable (no VM, no login, no game data, no build). They are kept apart on
purpose: a misconfigured VM must never read as a runtime regression.

## What it needs

- A libvirt Windows guest with **WinRM (5985)** and **SMB (445)** reachable, and a
  local admin login supplied through `WINVM_USER` / `WINVM_PASS` (never commit them).
  `WINVM_HOST` sets the guest IP; otherwise `WINVM_DOMAIN` (default `win11-dev`) is
  looked up with `virsh`.
- An **active interactive console session** on the guest. The game needs a desktop,
  which a WinRM process does not have, so it is started as an interactive scheduled
  task. Log in once at the console and leave it.
- The game installed at `C:\echovr` with its data under
  `_data\<version>\rad15\win10\packages`.
- On this host: `pywinrm`, `smbclient`, and `x86_64-w64-mingw32-gcc`.
- Optional, for `--dump-on-fail` analysis: `pip install minidump`.

## The rig

The test never touches the guest's own install. It builds `C:\nevr-systest\`: a copy
of `bin\win10` **without** `dbgcore.dll` (a legacy Echo Relay hijack DLL that NEVR
refuses to run beside) and without the stock `BugSplat64.dll`, junctions to the
shared `_data`/`content`/`sourcedb`, and its own `_local\config.json`. That config
points every service at `127.0.0.1:1` (connection refused), so the boot path runs
with no production contact and no credentials.

## Scenarios and checks

`gai` builds `tools/winvm/gai_probe.c` and times the exact `getaddrinfo` call that
`CBroadcaster::Initialize` makes (empty node string, flags 0, `AF_INET`/UDP).

`boot` deploys the DLL, launches `echovr.exe -noovr -server -headless -noconsole`,
observes for `--wait` seconds (default 90, minimum 45), then judges:

| Check | Fails when |
| --- | --- |
| `process_alive` | the game is not running at the end of the window |
| `no_modal_dialog` | the game owns a dialog window (its text is reported) |
| `no_fatal` | a `[FATAL]` / `ForceFatalExit` line was logged, or the exit code is non-zero |
| `hooks_installed` / `no_unexpected_hook_failure` | a required hook failed or was skipped; only explicit DIAG hooks and failures with the documented N126/N128/N127 statuses are warnings |
| `window_enumeration` / `window_pid` | live-process window enumeration lacks a fresh completion marker or is associated with another PID |
| `engine_progress` | the boot did not reach `--require-stage` (default `broadcaster`) |

`login` repeats the boot against the local Nakama using the seeded test account
and checks Nakama's session and `LoginSuccess` records. This tree does not
provide a local Nakama, so `login` and the default `all` scenario cannot run here.

Hook warnings require an explicit diagnostic tag or a matching failure reason,
status, and tracked exception context. A successful `All hooks installed` line
does not override a separate failure record.

`broadcaster` means `CBroadcaster::Listen` has been entered, which requires
`CBroadcaster::Initialize` (and the `getaddrinfo` inside it) to have returned. It
only appears in the ~30 s periodic `hook_liveness` report, hence the window.

Artifacts (`stdout.txt`, `windows.txt`, `results.txt`, the probe output) go to
`/var/tmp/work-nevr-runtime/winvm-<timestamp>/`, or `--out`.

## Things this cost time to learn

- **A modal dialog looks exactly like a hang** from outside: alive, `Responding`,
  near-zero CPU, no sockets, silent log. The first attempt mistook one for a
  `getaddrinfo` hang. `no_modal_dialog` enumerates the game's windows from inside the
  interactive session (`tools/winvm/enum_windows.ps1`) for this reason.
- `-noconsole` is rejected by the game unless `-headless` is also given (the dialog
  above is the legacy `dbgcore.dll` saying so).
- A `dbgcore.dll` in the game directory is fatal to the NEVR runtime unless
  `-allow-dbgcore` is passed. `--with-legacy-dbgcore` reproduces that configuration.
- The stock install was extracted one level too deep
  (`_data\_data\<version>\...`); the game then cannot find its packages. Preflight
  reports this and how to fix it (a directory junction).
- `_local\config.json` is optional (issue #21): every NEVR setting is in `config.yaml`,
  and a missing or unparseable `config.json` no longer stops a server. When present it
  is searched at `bin\win10\_local`, `bin\_local` and `<install root>\_local` (in that
  order); a `config.json` at the install root itself is never read. The offline `boot`
  rig still writes one, because its `*_host` keys are what point every service at
  `127.0.0.1:1`. The `login` rig writes none, so it also exercises the no-`config.json`
  boot.
- `hook_liveness ... CBroadcaster::ReceiveLocalEvent entries=0` is the **normal**
  signature of a server that never got a service session (the offline config here
  produces it). It is a symptom of "no dispatch happened yet", not evidence of a
  broken hook.

## Adding a check

Put the judging logic in `tools/winvm/checks.py` (pure, no I/O), add a fixture from
a real run under `tools/tests/fixtures/winvm/`, and a test in
`tools/tests/test_winvm_checks.py`; `just verify` runs those without a VM. Then
falsify it against the VM: a check that has never been seen failing proves nothing.

## When a run is stuck

`--dump-on-fail` fetches a full minidump; `tools/winvm/dump_stacks.py <dump>` resolves
thread stacks to module offsets (an echovr.exe VA per frame, ready for ReVault). Trust
only the top few frames: there is no unwind info, and deeper "return addresses" are
often stale stack data.

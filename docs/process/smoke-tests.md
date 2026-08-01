# Smoke tests

**Purpose:** find out what is *completely broken* — not what is subtly wrong. Each
check is one feature, one observable signal, one verdict. This is triage, not
validation: a PASS here means "the code path ran and reported success," not "this
feature is correct."

Two people run these. **The runtime operator** (agent, this host, Wine) runs
everything in the `boot`, `module`, `plugin`, and `server` groups. **Andrew** runs
the `client` group, because there is no automated client on this host and never
has been — which is exactly why client-side defects have survived so long.

## How a check is judged

Every check is scored from a captured log by `tests/smoke/score-log.sh`:

```sh
./tests/smoke/score-log.sh <logfile> [all|boot|module|plugin|server|client]
```

Three outcomes:

| Verdict | Meaning |
| --- | --- |
| **PASS** | the feature's success signal is in the log |
| **FAIL** | the feature's explicit failure signal is in the log |
| **ABSENT** | neither — the code path was never reached |

**ABSENT is not a pass, and it is not a failure.** It means the run did not test
the feature. Most IDs are ABSENT in any single run, by design — the run table
below says which run covers which IDs. Treating ABSENT as green is the exact
mistake this file exists to prevent.

The scorer's exit code is non-zero **only** on FAIL. ABSENT is a coverage gap,
reported but not an error.

### The signal table is ground truth, not source strings

Every pass-regex in the scorer was verified against real captured output. Source
format strings and real log lines drift apart: captured logs still say
`wave0 installing bug fix hooks` while current source says
`installing binary bug fix hooks` (renamed in N113). A signal copied from source
alone scores ABSENT against a real run and reads as a missing feature.

When a log string changes, fix the scorer and re-score a known-good log to prove
the row still fires. The scorer has been falsified in both directions: ten
synthetic failure signals each produced FAIL on the intended row, and a
known-good run produced 58 PASS.

Two signals are **Debug** level (`timer=high_resolution` among them) and are
filtered out at default verbosity. They are deliberately not in the table — a row
that can only ever be ABSENT is a row that cannot fail.

### Scoring `all` auto-detects the profile

Some rows are each other's negatives: `C02` fails on `host=server`, which is the
*correct* output for a server run. Scoring both profiles against one log therefore
manufactured a FAIL out of a healthy server. With no group argument the scorer
detects server vs client from the log and scores only the applicable profile,
reporting `groups = all (detected: server)`. Passing a group explicitly still
overrides, so a deliberate cross-check remains possible.

## Known environmental state — read before chasing a regression

**As of 2026-07-29, ServerDB registration fails on this host, and it is not a code
defect.** S23–S25, S36, S37 and S41 come back ABSENT: the server reaches
`Initialized game server`, starts loading level `0x3F9915D3001DC28E`, and never
finishes it. Instead it emits `[SYSNET] Setting low latency option for peer` for a
handful of implausible addresses, and sometimes takes an access violation
(`0xC0000005` at `game+0x14F47AE`, faulting pointer `0x7F7263FF0000`).

This was **measured, not assumed**, and the obvious hypothesis was disproved:

| Run | HEAD | Date | Registered? |
| --- | --- | --- | --- |
| `n105-shutdown` | `d654cd1` | 28 Jul | **yes** |
| `bisect-d654cd1` | `d654cd1` | 29 Jul | **no** |
| `smoke-r1` / `r1b` | `de1babf` | 29 Jul | **no** |

The **same commit** registered on 28 Jul and fails on 29 Jul. Eighteen commits
landed in between and none of them is responsible — the cause is outside this
repository (service-side; the owner was reworking the protobuf schema and the BSR
module on 29 Jul). Six earlier runs across four commits all registered cleanly, so
the 28 Jul behaviour is the norm, not a fluke.

**Do not bisect this.** Re-check it after the service side settles: if S23–S25 come
back on an unchanged tree, it was never ours. The protobuf contract is generated
from the BSR (`just proto` → `buf generate buf.build/echotools/nevr-api`); there is
no local proto checkout in this repo, so a schema change reaches us only through a
regenerate.

## Prerequisites

- Build present at `build/mingw-release/bin/` (`just build`).
- `echovr/_local/config.json` reachable from the game exe (searched up two parent
  dirs). Keys that gate whole features:
  - `nevr_socket_uri` — **without it the WebSocket bridge does not start and login
    injection cannot fire.** S16–S20 all go ABSENT.
  - `nevr_http_uri`, `nevr_http_key` — without them token auth is disabled (C03).
  - `telemetry_uri` — without it telemetry is disabled (S30).
- Patience: the game has a ~15–20 s splash delay before any NEVR code runs.
  **Judge nothing before 45 s.** `verify-server.sh` refuses run lengths under 45 s.

### Confounders to clear first

1. **The `plugins/` deploy gap.** The build emits `nevr_example.dll` into
   `build/mingw-release/bin/`, **not** `bin/plugins/`, and `bin/plugins/` does not
   exist. `launch-server.sh` copies `bin/plugins/*` — which matches nothing — so
   the deployed `echovr/bin/win10/plugins/` is empty and the plugin loader has
   never been exercised in any captured run. It logs
   `No plugins directory or no plugins found` every time. Plugin checks require
   **manual staging** (run R2).

   **Resolved by staging, 2026-07-29 (N119).** R2 and R3 have now been executed:
   staging `nevr_example.dll` by hand flips P02–P04 to PASS, and R3 confirms the
   N89 refusal fires without being over-broad. The loader and the ABI are sound;
   only the build/deploy wiring is missing. `nevr_example.dll` is left staged in
   `echovr/bin/win10/plugins/` so subsequent runs keep covering P01–P04 — if that
   directory is ever cleared, P02 goes back to FAIL and the cause is staging, not
   code.
2. **Stale DLLs in the build tree.** `build/mingw-release/bin/` still holds
   `anim_debugger.dll`, `broadcaster_bridge.dll` and `log_filter.dll` from 26–28
   Jul. The first two moved to the private `nevr-runtime-plugins` repo and the
   third is deliberately refused by the loader. **Do not stage them** except in R3,
   where refusing `log_filter.dll` is the thing under test.
3. **Never grep a live run.** grep block-buffers and the run looks hung. Capture to
   a file, score the file.

## Run table

Runs are ordered so that each one's prerequisites are already satisfied. IDs not
listed for a run are expected ABSENT in it.

| Run | Who | Command | Covers | Notes |
| --- | --- | --- | --- | --- |
| **R1** server baseline | operator | `./verify-server.sh smoke-r1 default 100` | B01–B14, M01–M06, S01–S28, S31–S41 | The workhorse. One run, ~55 checks. Expect P02 FAIL until R2. |
| **R2** plugin loader | operator | stage `nevr_example.dll` into `echovr/bin/win10/plugins/`, then `./verify-server.sh smoke-r2 default 100` | P01–P04 | First-ever runtime evidence for the plugin path and API v3 caps. |
| **R3** loader refusal | operator | additionally stage `log_filter.dll`, rerun | P05 | Negative control for the loader: it must *refuse* this DLL (N89). Remove it afterwards. |
| **R4** UPnP | operator | server run with `-upnp` added | S29 | Needs a `-upnp` flag-set in `verify-server.sh` or a manual invocation. Result depends on the LAN's IGD — a router without UPnP is a legitimate FAIL of the environment, not the code. |
| **R5** telemetry | operator | `telemetry_uri` set + a listener, server run | S30 | Without a listener this is ABSENT, not FAIL. |
| **R6** client baseline | **Andrew** | `./launch-client.sh 2>&1 \| tee /var/tmp/work-nevr-runtime/client-r6.log` | C01, C02, C03, C09, C10, C11 | Also the negative control for S02: a client must show `SET(WINDOWED)`, never `CLEAR(HEADLESS)`. |
| **R7** fresh device auth | **Andrew** | delete the cached credentials, then launch client | C04, C05, C06 | Watch for the ASCII code box; complete the flow in a browser. |
| **R8** cached-token restart | **Andrew** | relaunch client without deleting credentials | C07 | Must **not** re-prompt. A second prompt is a FAIL even if login succeeds. |
| **R9** token refresh | **Andrew** | hand-edit the cached expiry to the past, keep the refresh token, relaunch | C08 | The only practical way to force this without waiting out a real expiry. |
| **R10** live session | **Andrew** | join a match, change a loadout, leave | C12, C13 | Plus the visual checks below, which no log can judge. |

`verify-server.sh` already prints its own summary (exit code, window census,
crash-dump delta). Score its log afterwards:

```sh
./tests/smoke/score-log.sh /var/tmp/work-nevr-runtime/server-runs/smoke-r1/server.log
```

For R6–R10, Andrew captures to a file and the operator scores it with
`... client-r6.log client`.

## Checks with no log signal

The scorer cannot judge these. They need eyes, and R10 is when to look.

| # | Check | Pass looks like |
| --- | --- | --- |
| V1 | Headless server opens no window | `verify-server.sh` reports `max_game_windows=0` (it attributes windows by PID, so an unrelated desktop window cannot confound it) |
| V2 | Cosmetic tints actually render | a CDN-delivered tint is visible on a player model, not merely `tints loaded` in the log |
| V3 | `DSC-` prefix in client UI | account IDs display as `DSC-…`, never `PSN-` or `UNK-` |
| V4 | Match is joinable and playable | the client reaches a live session on a NEVR server and gameplay is smooth |
| V5 | Idle CPU is sane | measure with the server idle; the open question is whether idle CPU and ping latency are one fix or two — **do not answer it from one run** |
| V6 | Clean exit leaves no zombie port | the server's port is immediately re-bindable after CTRL+C |
| V7 | No crash dumps | `verify-server.sh` reports `crash_dumps_after` equal to `crash_dumps_before` |

## Not testable here, and why

State these as untested rather than letting an empty result read as green.

| Feature | Status |
| --- | --- |
| Broadcaster guard | **Not implemented.** `BroadcasterGuard::Install()` is an empty placeholder and logs `no-op placeholder, nothing installed`. Nothing to smoke-test. |
| Quest / standalone | **Stub.** `src/standalone/` does not build; awaiting the reconstruction. |
| Build identity / attestation at login | **Not built.** The login JSON still hard-codes `buildversion:631547` and an empty `publisher_lock` (`src/runtime/compat/ws_bridge.cpp`). This is the open N112 work; there is no signal to test yet. |
| Plugin manifest transmission | **Not built.** Also N112. Capability *declaration* exists (v3); *transmission* does not. |
| Go integration suites | **Excised** from `just verify` (RULINGS.md 2026-07-20 "Test harness excised"). Present under `tests/system/` and `tests/plugins/` but not part of any gate. |
| `crash-handler` plugin | Present in `plugins/` but absent from the AGENTS.md plugin table and not built by `plugins/CMakeLists.txt` (only `common` and `example` are). Unwired — resolve before smoke-testing it. |

## Recording results

Score into a file per run and keep the verdicts with the run, not in prose:

```sh
./tests/smoke/score-log.sh <log> > /var/tmp/work-nevr-runtime/server-runs/<run>/smoke-score.txt
```

A FAIL becomes a `BUGS.md` entry in the `N`-prefix namespace with its evidence
rank (`docs/standards/verification.md`). A run-to-run ABSENT that *should* have
been covered is a gap in this file — fix the run table, not the verdict.

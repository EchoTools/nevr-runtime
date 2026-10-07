# NEVR Runtime Logging Standards

_Authored by @agents._

**Required reading** for ANY agent writing, reviewing, or modifying code
that produces log output in the nevr-runtime repository. Read this BEFORE
adding a `Log()` call, BEFORE reviewing a PR that touches logging, and
BEFORE tuning the built-in log filter.

---

## The Log Line Knows What It Is

> _The log line knows what it is, because it carries what it isn't: silence._

This document is structured by negation as much as assertion. A log line
that is missing its identifier is not a log line — it is noise. A
subsystem that produces no log lines is not healthy — it is silently
broken. The "Never" and "Hard Stops" clauses are load-bearing: they are
how an agent triangulates a correct log line.

### This IS

- A binding ruleset for every `Log()` call in `src/`, `plugins/`,
  `modules/`, and any future component that links `libcommon.a`.
- A review gate: the "Hard Stops" table at the end of this document is
  enforced. A log line that fails any check is rejected in review.
- A definition of what constitutes noise (see N18) and what a log line
  SHALL carry to be actionable.
- The single authority on log level usage in this project. If you are
  unsure whether something is INFO or DEBUG, the answer is here.

### This is NOT

- A tutorial on the `Log()` function signature. That is in
  `src/core/logging.h` and `AGENTS.md`. This document defines WHAT
  goes in the format string, not how to call the function.
- A style preference. The "Hard Stops" are not negotiable; they are the
  mechanical difference between a log that diagnoses a problem and one
  that wastes the operator's time.
- Optional for "debug prints" or "temporary" log lines. Every log line
  in every `.cpp` and `.h` file is in scope. If it ships, it must meet
  the standard.

### You SHALL

- You **shall** use `Log(EchoVR::LogLevel::level, "format", ...)` as the single entry
  point. No `printf`, no `fprintf`, no `cerr`, no `OutputDebugString`,
  no `std::cout`. (`logging.h:31`, `CPP-MINGW-ADDENDUM-GENERIC.md` "Logging (Structured, Always)").
- You **shall** include a subsystem tag on EVERY log line. The tag identifies which
  NEVR component produced the line. See the Subsystem Tags table below.
- You **shall** log the outcome. A log line that says "connecting" without a
  corresponding "connected" or "connection failed" is incomplete.
- You **shall** log the relevant identifier. Connection index, session ID, XPID
  string, server ID, or request ID — there is always one identifier
  that the operator needs to trace the event through the system.
- You **shall** use the correct log level per the Level Guidelines below. When in
  doubt, go one level higher (DEBUG -> INFO, INFO -> WARNING) — a
  too-verbose log can be filtered; a missing log cannot be recovered.
- You **shall** log identity at login. Every login/acquisition path SHALL include the
  full platform-identity string (XPID) as a structured field (see Rule 2).

### You SHALL NOT

- You **shall not** log a bare free-text message with no subsystem tag and no identifier.
  `Log(Info, "Connected")` is not a log line — it is a riddle.
- You **shall not** log at INFO level inside a hot path (per-frame, per-tick,
  per-connection). Use DEBUG for the narrative detail; emit one INFO
  summary line instead (Rule 12).
- You **shall not** log a state transition without also logging the state it transitioned
  FROM and TO. "State changed" without the old and new states is
  useless.
- You **shall not** log an error without the error code. `"Failed to connect"` without
  the `WSAGetLastError` or HRESULT is not actionable.
- You **shall not** suppress a hook installation failure silently. Hook failures are
  WARNING-level events with the target address, expected prologue, and
  actual bytes (see Rule 5).
- You **shall not** log raw binary data, hex dumps, or pointer values at INFO level.
  These are DEBUG-level diagnostics and SHALL be gated by a verbosity
  flag or compile-time guard.
- You **shall not** use `Log(Debug, ...)` for anything you expect an operator to need
  during a production incident. DEBUG is off by default in production
  builds. If an operator needs it, it is INFO.
- You **shall not** leave a `// TODO: add logging` comment without a N-ledger
  entry. A TODO without a ticket is a wish.

---

## Subsystem Tags

Every log line begins with a bracketed tag identifying the emitting
component. Tags are hierarchical: `[NEVR.COMPONENT]` for NEVR-authored
code, `[COMPONENT]` (no NEVR prefix) for third-party or game-native
subsystems that NEVR annotates.

| Tag                   | Component                                     |
| --------------------- | --------------------------------------------- |
| `[NEVR.WS]`           | ws-bridge (WebSocket proxy, login injection)  |
| `[NEVR.GAMESERVER]`   | GameServerLib (lobby registration, sessions)  |
| `[NEVR.PATCH]`        | gamepatches (boot hooks, config, CLI, mode)   |
| `[NEVR.HEADLESS]`     | Headless graphics stub, render-skip patches   |
| `[NEVR.MODULE]`       | Module loader (LoadModule, drop-in modules)   |
| `[NEVR.PLUGIN]`       | Plugin loader (discovery, lifecycle)          |
| `[NEVR.TELEMETRY]`    | Telemetry streamer (WebSocket, snapshots)     |
| `[NEVR.XPID]`         | Platform-identity patches (DSC provider)      |
| `[NEVR.CONFIG]`       | Config loading, service redirects             |
| `[NEVR.CRASH]`        | Crash recovery, dump, longjmp                 |
| `[NEVR.AUTH]`         | Token acquisition, device-code flow, refresh  |
| `[NEVR.CDN]`          | Asset CDN download and override              |
| `[NEVR.HTTP]`         | WinHTTP/curl bridge                          |
| `[NEVR.UPNP]`         | UPnP port mapping                            |
| `[NEVR.RESOURCE]`     | Resource override / embedded asset injection |
| `[NEVR.LOGFILTER]`    | The log filter's own health and rate summary |
| `[NEVR.DLLHOOK]`      | LoadLibrary interception                     |
| `[NEVR.PROFILE]`      | Server profile / memory snapshot             |
| `[NEVR.FATAL]`        | Fatal-error path (ServerFatal)               |
| `[NEVR.SCENARIO]`     | Scenario-test control endpoint (test builds only; never in a release DLL) |
| `[server_timing]`     | Server tick-rate / timing patches             |

**Rule:** If you add a new component, add its tag to this table. If you
are unsure which tag to use, use the nearest existing tag and note it
for review.

---

## Level Guidelines

| Level   | When to Use                                                                 | Production |
| ------- | --------------------------------------------------------------------------- | ---------- |
| ERROR   | Something is broken and the operator SHALL act. Crash dump, protobuf parse   | Always on  |
|         | failure, module init failure, unrecoverable state.                          |            |
| WARNING | Degraded but running. Hook install failure, config key missing, login       | Always on  |
|         | failure (retryable), retry attempt, suppressed NoNetwork transition.        |            |
| INFO    | **Summary, not narrative.** One line per event that tells the operator       | Always on  |
|         | WHAT happened and whether it succeeded. Startup complete, connection         |            |
|         | established, login succeeded, config loaded, plugin set loaded (count).      |            |
|         | The per-item detail lives at DEBUG; INFO gets the aggregate.                |            |
| DEBUG   | **Narrative detail.** Per-item descriptions, per-request data, on-disk      | **Off**    |
|         | state, pointer values, hex dumps, per-frame counters, per-connection         |            |
|         | diagnostics. The individual boot-step lines (`[NEVR.BOOT] installing X`)    |            |
|         | live here; INFO gets the summary (`boot complete: N hooks`).                |            |

**Anti-patterns by level:**

- **ERROR used for routine failures:** A login rejection with a
  well-defined error code is WARNING, not ERROR. ERROR means "the
  process may not be able to continue." A retryable network timeout is
  WARNING, not ERROR.
- **INFO used for per-item detail:** Individual hook installs,
  per-connection TLS diagnostics, per-frame counters, hex dumps of
  game state — these are DEBUG. INFO is the summary, not the
  narrative. If an operator needs the per-item breakdown, they enable
  DEBUG.
- **WARNING used for expected conditions:** "No plugins directory
  found" on a fresh install is INFO, not WARNING. WARNING means
  "something is not right but the system can cope."
- **DEBUG used for state transitions:** "Connected to ServerDB" is a
  state transition. It is INFO, not DEBUG. The operator needs to know
  when connectivity is established or lost.

---

## Rules

### Rule 1: Every log line is a structured claim

Every log line carries four fields. A log line missing any of these is
**naked** and is noise.

1. **WHAT** — the event that occurred (connected, disconnected, failed,
   registered, injected, redirected, loaded, crashed).
2. **WHERE** — the subsystem tag (see Subsystem Tags table).
3. **IDENTIFIER** — the traceable entity (connection index, session ID,
   XPID, server ID, request ID, module name, URI).
4. **OUTCOME** — the result (success/error code, bytes transferred, old
   state -> new state, duration, count).

```cpp
// BEFORE (naked — no identifier, no outcome)
Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Connected to server");

// AFTER (carries WHAT=connected, WHERE=[NEVR.GAMESERVER], ID=uri, OUTCOME=success)
Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] websocket connected uri=%s conn_id=%d",
    uri, connIndex);
```

**Where:** This rule applies to every `Log()` call site. Existing
violations are recorded in N19 (no logging standards exist).

### Rule 2: XPID shall be logged at login

The login/acquisition path SHALL log the full platform-identity string
being used. The XPID is the external identifier that ties a log session
to a specific user account — it is the single most important identifier
in the log.

```cpp
// BEFORE (N15 — numeric account ID only, no platform prefix, no full XPID)
Log(EchoVR::LogLevel::Info,
    "[NEVR.WS] Injected LoginRequest (OVR-ORG-%llu, %zu bytes)",
    (unsigned long long)discordId, loginMsg.size());

// AFTER — full XPID string, connection index, byte count
std::string xpid = platformPrefix + "-" + std::to_string(accountId);
Log(EchoVR::LogLevel::Info,
    "[NEVR.WS] login injected xpid=%s platform=%d conn=%d size=%zu",
    xpid.c_str(), platformCode, connIdx, loginMsg.size());
```

The platform prefix SHALL be derived from the actual platform code in the
login payload, not hardcoded. Platform codes are the game's own
1-indexed numbering (STM=1, DSC=2, XBX=3, OVR_ORG=4, OVR=5, BOT=6, DMO=7);
code 2 is "PSN" in the game's string table and reads "DSC" after the runtime rewrites it.
The bridge logs in as OVR_ORG (code 4), so its XPID is `OVR-ORG-<id>`; a
login as DSC (code 2) would produce `DSC-<id>`.

**Where:** the injection site is `InstallWebSocketBridge` in
`src/runtime/compat/ws_bridge.cpp` (the `login injected xpid=` log line). Tracked as N15.

### Rule 3: Silence is not success

A subsystem that produces no log lines is indistinguishable from one
that never ran. Every significant state transition SHALL produce a log
line at the point of transition.

The following transitions are **mandatory** INFO-level log points:

- **Startup:** component initialized, hooks installed, config loaded,
  modules/plugins loaded (each with name and version).
- **Connectivity:** WebSocket connected (with URI and connection index),
  WebSocket disconnected (with numeric close code), WebSocket error (with
  numeric HTTP status/retry fields where available).
- **Registration:** registration request sent (with message type and
  size), registration success received (with server_id),
  registration failure received (with error code and message byte count).
- **Login:** login request injected (with XPID, connection index, and
  size), login success received, login failure received (with status
  code and message byte count).
- **Session:** session created, session started, session ended (each
  with session ID).
- **Shutdown:** component shutting down, hooks removed, connections
  closed.

A component that initializes silently is a component whose failure is
undetectable. Every `Init()` function SHALL log at entry and exit, with
the exit log including success/failure and any relevant state.

### Rule 4: Remote text and response bodies are not log fields

HTTP response bodies, JSON parser exception text, protobuf error messages,
WebSocket close/error reasons, and callback exception text can echo secrets or
user supplied content. Do not log these values, even at Debug. Keep the
operational signal by logging numeric status/error codes, bounded byte counts,
retry counts, and stable event markers. URL diagnostics must use the redacted
formatters and omit query and fragment data. Preserve the original response
and request bytes for protocol handling; this rule changes diagnostics only.

### Rule 5: Noise is a defect

echovr-native log lines are "objectively 97% worthless" (owner). The
built-in log filter exists to suppress them (see N18). If noise is
reaching the production log, the filter is broken and that is a defect.

What constitutes noise:

- **Repeated identical lines.** A log line that appears more than once
  per second with identical content is noise. Use rate-limited summary
  logging instead ("X repeated N times in the last T seconds").
- **Lines with no structured fields.** A log line that carries only a
  free-text message with no identifier, no subsystem tag, and no outcome
  is noise. See Rule 1.
- **Game-native lines that NEVR doesn't annotate.** Lines from
  `echovr.exe` that pass through unmodified, without a NEVR subsystem
  tag or structured wrapper, are noise. The log_filter SHALL suppress
  these in production server builds.
- **Per-item detail at INFO level.** Per-frame, per-tick, or per-connection
  diagnostics at INFO are DEBUG. INFO gets the summary; DEBUG gets the
  narrative (Rule 12).
- **Hex dumps, pointer values, and raw binary at INFO level.** These
  are DEBUG, gated by a verbosity flag.

**Filter audit checklist (N18 fix direction):**
1. Verify the built-in log filter is capturing game lines (its health line
   reports `game_lines=`; a zero-game-lines warning names its cause: hook not
   installed, hook target taken by another module, or the game idle or blocked)
   configuration.
2. Capture a representative server log from a live session.
3. Count lines per subsystem tag; any tag with >50% of total lines is a
   candidate for rate-limiting or DEBUG demotion.
4. Identify any game-native line (no `[NEVR.*]` prefix) appearing more
   than once per 10 seconds — add a suppression rule.
5. After tuning, re-capture and verify that >80% of lines carry a NEVR
   subsystem tag and a traceable identifier.

### Rule 6: Hook failures are WARNINGS, not silent drops

Hook installation failures (wave0, MinHook, prologue validation) SHALL
be logged at WARNING level with:

- The target address (hex VA).
- The expected prologue bytes (hex).
- The actual bytes at the target (hex).
- The hook name or purpose.

```cpp
// BEFORE (insufficient — missing expected/actual bytes)
Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to hook function at 0x%llX", targetVa);

// AFTER
Log(EchoVR::LogLevel::Warning,
    "[NEVR.PATCH] hook failed name=%s va=0x%llX expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x",
    hookName, targetVa,
    expected[0], expected[1], expected[2], expected[3],
    actual[0], actual[1], actual[2], actual[3]);
```

At boot completion, aggregate all hook failures into a single summary
line:

```
[NEVR.PATCH] hooks installed: %d succeeded, %d failed (failed: %s)
```

Failures that are benign (target address changed between binary builds,
function removed) SHALL still be logged; an operator seeing a new failure
in the summary cannot distinguish "this was always failing" from "this
just started failing" without a baseline.

**Where:** `src/runtime/patch/binary_bug_fixes.cpp:435-439`,
`src/runtime/patch/headless_graphics.cpp:454-525`,
`src/runtime/patch/resource_override.cpp:134`,
`src/runtime/log/builtin_filter.cpp:826`. Tracked as N17
(startup hook errors not systematically tracked).

### Rule 7: The Log() function is the single entry point

`Log(EchoVR::LogLevel::level, "format %d", val)` from
`src/core/logging.h:18` is the mechanism. This standard defines WHAT
goes in the format string and what level to use. No other output
mechanism is permitted.

```cpp
// Good — uses Log(), carries all four fields
Log(EchoVR::LogLevel::Info,
    "[NEVR.GAMESERVER] registration response server_id=%llu ip=%s",
    (unsigned long long)serverId, ipAddress.c_str());

// Good — uses FatalError for unrecoverable termination
FatalError("Required module missing", "ws_bridge");

// Bad — never do this
printf("registration response server_id=%llu\n", serverId);
fprintf(stderr, "error: %s\n", what());
OutputDebugStringA("got here");
std::cerr << "failed" << std::endl;
```

### `Log()` does not emit JSON, and that was a decision — not an omission

`FormatJsonLogEntry` exists in `src/core/logging.cpp:70` and is called from
nowhere in production (the only other reference is a test stub). It is not
"not yet wired": it WAS wired, and was deliberately unwired.

  a658d42  2026-02-09  added it, and called it from Log()
  6c0369f  2026-03-24  removed that call; Log() now routes to the game's own
                       EchoVR::WriteLog, falling back to vfprintf(stderr) only
                       before the game logger exists

So NEVR lines go through the game's logger and appear in its stream, rather than
being emitted as a second, parallel JSON format. Do not "finish" the JSON path on
the assumption it was left half-done — it was superseded four months ago, and
re-wiring it would double every log line.

**Structured JSONL does ship, from a different place**: the built-in filter writes
a per-run JSONL file (`src/runtime/log/builtin_filter.cpp`), and its schema is NOT
the one `FormatJsonLogEntry` produces — it carries a `run` field and has no
`caller` field.

### Rule 8: State transitions log FROM -> TO

Every state machine transition SHALL log both the old state and the new
state. The operator cannot diagnose a stuck state machine from a log
that only says "state changed."

```cpp
// BEFORE (no FROM state)
Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Session state changed to: started");

// AFTER
Log(EchoVR::LogLevel::Info,
    "[NEVR.GAMESERVER] session state transition from=%s to=%s session_id=%s",
    oldStateName, newStateName, sessionId.c_str());
```

This applies to:
- NetGame state transitions (loading root -> logging in -> logged in ->
  loading global -> lobby).
- WebSocket connection state (disconnected -> connecting -> connected ->
  disconnected).
- Session lifecycle (created -> active -> ended).
- Game mode transitions (lobby -> pregame -> playing -> postgame ->
  lobby).

### Rule 9: Connection-scoped events carry the connection index

Every log line that relates to a specific WebSocket connection SHALL
include the connection index (`conn=%d`). Connections are identified by
index in the ws-bridge (0 = config connection, 1+ = game login
connections). Without the index, an operator cannot correlate the
connect, message, and disconnect events for a single connection.

```cpp
// BEFORE (no conn index — which connection?)
Log(EchoVR::LogLevel::Info, "[NEVR.WS] Remote open: %s", uri.c_str());

// AFTER
Log(EchoVR::LogLevel::Info, "[NEVR.WS] remote opened conn=%d uri=%s",
    connIdx, uri.c_str());
```

### Rule 10: Error paths log the error code

Every ERROR or WARNING log line that reports a failure SHALL include the
error code that caused it. `"Failed to load module"` without the
`GetLastError()` value is not actionable — the operator cannot
distinguish "file not found" from "access denied" from "out of memory."

```cpp
// BEFORE (no error code)
Log(EchoVR::LogLevel::Error, "[NEVR.MODULE] Failed to load %s", name);

// AFTER
Log(EchoVR::LogLevel::Error,
    "[NEVR.MODULE] load failed name=%s error=%lu path=%s",
    name, GetLastError(), dllPath.c_str());
```

### Rule 11: Config decisions logged at load time

Every config decision that affects runtime behavior SHALL be logged at
INFO level when it is loaded. Log the key and a safe summary, not secret
values or unredacted URLs. This includes:

- CLI flags (`-server`, `-headless`, `-timestep`, `-telemetry`).
- Config-file overrides (arena round time, mercy score, and redacted service URLs).
- Service redirects (each key and redacted source/destination URL).
- Module/plugin load decisions (loaded, skipped, failed).

The log is the only record of what configuration decisions the process is
running with. If a decision is not logged, the operator must guess whether
it was applied.

### Rule 12: Bootstrap log lines carry a level prefix

Before the game logger is available, `BootLogTee::TeeFprintf` is the
only output mechanism. These lines SHALL embed their level in the format
string so they are distinguishable from timestamped `Log()` output:

```cpp
// BEFORE (no level — indistinguishable from any other bootstrap line)
BootLogTee::TeeFprintf("[NEVR.BOOT] installing crash recovery hooks...\n");

// AFTER
BootLogTee::TeeFprintf("[NEVR.BOOT] info; installing crash recovery hooks...\n");
```

The level prefix (`info;`, `warn;`, `error;`) bridges the gap until
`Log()` becomes available. Once the game logger is initialized, all
subsequent output SHALL use `Log()` — `TeeFprintf` is a bootstrap
mechanism only.

### Rule 13: INFO is summary, DEBUG is narrative

Every event that produces multiple log lines SHALL follow this pattern:

- **INFO:** one line that says WHAT happened and the outcome. The
  operator reads INFO to understand system state at a glance.
- **DEBUG:** the per-item narrative that explains HOW it happened.
  Individual steps, measurements, and decisions that the operator only
  needs during deep diagnosis.

```
// INFO — one summary line
[NEVR.PATCH] boot complete: 14 hooks installed, 1 deferred, 1 known-failed
  (target address changed in a prior game update — see N126/N128 for history), 0 unexpected

// DEBUG — per-item narrative (gated behind DEBUG level)
[NEVR.BOOT] debug; installing crash recovery hooks
[NEVR.PATCH] debug; CreateProcessA hook installed (crash reporter disabled)
[NEVR.PATCH] debug; CreateProcessW hook installed (crash reporter disabled)
[NEVR.PATCH] debug; ExitProcess hook installed (prevents crash reporter termination)
...
```

This applies to boot sequences, plugin loading, connection setup, and
any multi-step operation. If there are N items, INFO gets one summary
line with the count; DEBUG gets the N individual lines.

### Rule 14: Sensor-encoded level decisions are revisable

Sensors that pin a log level (e.g. "this message SHALL be Debug") encode
a past decision, not a permanent law. When the logging standard evolves,
a sensor that contradicts the current standard is a sensor that needs
updating — not a reason to violate the standard.

If you change a log level and a sensor blocks it, update the sensor in
the same commit. The sensor exists to prevent *drift*, not to prevent
*deliberate revision*.

---

## BEFORE / AFTER Reference

These examples are drawn from actual code in the repository. The BEFORE
column shows the current log line; the AFTER column shows the corrected
form.

| Context | BEFORE | AFTER |
| ------- | ------ | ----- |
| Login injection (N15) | `"[NEVR.WS] Injected LoginRequest (OVR-ORG-%llu, %zu bytes)"` | `"[NEVR.WS] login injected xpid=%s platform=%d conn=%d size=%zu"` |
| WebSocket connected | `"[WEBSOCKET] Connected to ServerDB"` | `"[NEVR.WS] websocket connected uri=%s conn=%d"` |
| WebSocket disconnected | `"[WEBSOCKET] Disconnected from ServerDB (code: %d, reason: %s)"` | `"[WEBSOCKET] Disconnected from ServerDB (code: %u) reconnect_count=%u"` |
| Login success | `"[NEVR.WS] LOGIN SUCCESS"` | `"[NEVR.WS] login success xpid=%s conn=%d session=%s"` |
| Login failure | `"[NEVR.WS] LOGIN FAILURE: status=%llu msg=%.*s"` | `"[NEVR.WS] login failed status=%llu message_bytes=%zu"` |
| Hook failure | `"[wave0] FAILED to hook fcn.0x%llX"` | `"[NEVR.PATCH] hook failed name=%s va=0x%llX expected=%s actual=%s"` |
| Config redirect | `"[NEVR.PATCH] Service redirect [%s]: %s -> %s"` | `"[NEVR.PATCH] service redirect key=%s from=%s to=%s"` (URLs redacted) |
| Module loaded | `"[NEVR.MODULE] Loaded: %s"` | `"[NEVR.MODULE] loaded name=%s path=%s"` |
| Plugin loaded | `"[NEVR.PLUGIN] Loaded: %s v%u.%u.%u (API v%u)"` | Already compliant — carries name, version, API version |
| Registration | `"[NEVR.GAMESERVER] Received registration success via protobuf: server_id=%llu, ip=%s"` | Already compliant — carries server_id, ip |
| State transition | `"[NETGAME] Logging in..."` | `"[NEVR.PATCH] netgame state from=%s to=%s"` |
| Crash dump | `"=== CRASH DUMP ==="` | `"[NEVR.CRASH] exception code=0x%08lX name=%s rip=0x%llX"` |

---

## Log Volume Budget (per-subsystem, per-event)

| Event                    | Level | Max frequency    | Notes                                       |
| ------------------------ | ----- | ---------------- | ------------------------------------------- |
| State transition         | INFO  | Per-transition   | Log FROM -> TO, once per change             |
| WebSocket connect        | INFO  | Once             | Log URI + conn index                        |
| WebSocket disconnect     | INFO  | Once             | Log numeric close code + reconnect count     |
| WebSocket error          | WARN  | Once per failure | Log numeric status/retries + reconnect count |
| Message forward          | DEBUG | Rate-limited     | Summary every N seconds or N messages        |
| Per-frame diagnostic     | DEBUG | Off in prod      | Gated by verbosity flag                     |
| Login injected           | INFO  | Once per conn    | Log XPID + conn + size                      |
| Registration request     | INFO  | Once             | Log message type + size                     |
| Registration response    | INFO  | Once             | Log server_id + status                      |
| Hook failure             | WARN  | Once per failure | Log VA + expected + actual                  |
| Config value loaded      | INFO  | Once per key     | Log key + value                             |
| Module/plugin loaded     | INFO  | Once per module  | Log name + path/version                     |
| Crash / exception        | ERROR | On crash         | Log code + RIP + register dump              |
| Memory / pointer value   | DEBUG | Off in prod      | Gated by verbosity flag                     |
| Hex dump                 | DEBUG | Off in prod      | Gated by verbosity flag                     |

---

## Hard Stops

These are enforced in code review. A PR that introduces a log line
failing any of these checks is rejected until the violation is fixed.

| Problem                                      | Why It's a Stop                   | Fix                                                   |
| -------------------------------------------- | --------------------------------- | ----------------------------------------------------- |
| No subsystem tag                             | Can't trace to component          | Add `[NEVR.COMPONENT]` prefix                         |
| No identifier                                | Can't correlate events            | Add conn=%d, xpid=%s, session_id=%s, or equivalent    |
| No outcome                                   | Can't tell if it worked           | Add success/error code, state transition, or count    |
| XPID not logged at login (N15)               | Can't identify connecting user    | Log the full XPID string                              |
| Hook failure at DEBUG or not logged          | Silent regression in coverage     | Log at WARNING with VA + expected + actual            |
| State transition without FROM state          | Can't diagnose stuck state        | Log old_state -> new_state                            |
| Error without error code                     | Not actionable                    | Add GetLastError(), HRESULT, or status code           |
| INFO in any hot path or per-item detail       | Floods the log                    | Demote to DEBUG; emit one INFO summary line instead.  |
| printf/fprintf/cerr instead of Log()         | Bypasses structured logging       | Use Log() from logging.h.  Exception: `BootLogTee::TeeFprintf` before the game logger exists (Rule 11). |
| Game-native line without NEVR annotation     | Noise (N18)                       | Suppress or wrap with structured fields               |
| Free-text message with no key=value fields   | Not machine-parseable             | Use key=value format for identifiers and outcomes     |
| Config value not logged at load              | Configuration is invisible        | Log at INFO with key + value                          |
| Event with no consequence stated (G)         | Reader can't tell why it matters  | State the "so what," not just the "what"               |
| Field name doesn't match its type (H)        | Misleads at a glance, invites bugs | Rename the field or fix the representation             |
| Raw hex/pointer/hash at INFO+ unresolved (I) | No human meaning at that level    | Resolve to a name, or demote to DEBUG                  |
| Same fact stated twice (J)                   | Wastes the reader's attention     | Merge into one line, or delete the redundant one        |
| Ticket ref standing in for an explanation (K)| Reader must leave the log to understand | Put the explanation in the line; ticket is a footnote |
| Message doesn't parse as English (L)         | Actively confusing                | Reread it as a sentence before shipping                |
| Same failure code explained inconsistently across sites (M) | Reader can't tell benign from urgent | Bring every site up to the best existing explanation |

---

## Message Content Quality (Categories G-M)

Everything above this section governs LEVEL (is this INFO or WARNING?) and
structure (does it have a tag, an identifier, an outcome?). A log line can
satisfy every rule above and still be useless: it can state that an event
happened without saying why an operator should care, or dump a raw hex value
a reader can't act on. This section is a second, orthogonal pass — assume
the level is already right and Rule 1's four fields are already present, and
ask instead: **does the line's CONTENT actually tell the reader what they
need?**

This section was added after a 2026-09 repo-wide audit of every `Log()` /
`FatalError()` / `ServerFatal()` call site (706 sites across `src/`,
`src/modules/`, and `plugins/`) found that ~55% of flagged sites failed
Category G alone — the single most common defect in this codebase's logging
is not a missing tag or a wrong level, it's a line that reports an event
without reporting its consequence. Apply these checks to every new `Log()`
call, the same way Rule 1-13 already apply.

### Category G: State the consequence, not just the event

"X happened" is not the same claim as "X happened, and here is why it
matters." A log line SHALL answer "so what?" in the line itself — a reader
should never need to already know why a hook/patch/handler exists to
understand why its line is there.

```cpp
// BEFORE — an event with no consequence
Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateProcessW hook installed");

// AFTER — the same event, with the reason it exists
Log(EchoVR::LogLevel::Info,
    "[NEVR.PATCH] CreateProcessW hook installed (crash reporter launch blocked)");
```

This is distinct from Rule 1's OUTCOME field (success/fail/count/bytes) —
OUTCOME says whether the event succeeded; Category G says why the event was
worth doing at all. A line can have a perfect OUTCOME and still fail G:
`"hook installed: 1/1 succeeded"` has an outcome, but not a consequence.

### Category H: Match the field's name to what it actually holds

A field name is a promise about type. `_id` implies an opaque identifier, not
a display string. A bare `=1`/`=0` reads as a count or a flag with no way to
tell which. A "count" field that's secretly a bitmask will eventually get
compared with `==` by someone who trusted the name.

```cpp
// BEFORE — success is a bool, printed as if it were a count
Log(EchoVR::LogLevel::Debug, "[NEVR.WS] frame forwarded success=%d", ok);

// AFTER — the name and the representation agree
Log(EchoVR::LogLevel::Debug, "[NEVR.WS] frame forwarded success=%s", ok ? "true" : "false");
```

Watch especially for the same field NAME used for two different semantic
TYPES across nearby lines in the same file (e.g. `conn=%d` meaning a
login-order index in most of a file, then `conn=%s` meaning a third-party
library's internal connection-id string a few hundred lines later) — that's
Category H even when each individual line is internally consistent.

### Category I: Resolve mechanism dumps to human meaning

Raw hex, pointer values, symbol hashes, or virtual addresses at INFO or
above, with nothing resolved for a reader who isn't the hook's original
author, are noise wearing the clothes of data.

```cpp
// BEFORE — a real finding: hardcoded placeholder bytes presented as real data
Log(EchoVR::LogLevel::Warning,
    "[NEVR.LOGFILTER] hook verify mismatch expected=00000000 actual=%02x%02x%02x%02x status=%d",
    actual[0], actual[1], actual[2], actual[3], status);

// AFTER — the real expected bytes, and a resolved status name
Log(EchoVR::LogLevel::Warning,
    "[NEVR.LOGFILTER] hook verify mismatch expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x status=%s",
    expected[0], expected[1], expected[2], expected[3],
    actual[0], actual[1], actual[2], actual[3], MH_StatusToString((MH_STATUS)status));
```

If a value genuinely can't be resolved to a name (a truly novel symbol hash
with no corpus entry) and has no operational meaning to anyone but the hook
author, that's a real signal too — but the signal is "this belongs at
DEBUG," not "log it at INFO anyway because it's technically data."

### Category J: One fact, one line

Two or more lines stating the same fact twice — a per-item line immediately
followed by a summary repeating the identical count with nothing new, or a
capstone line that adds nothing after the line before it already said it —
SHALL be merged or deleted.

```cpp
// BEFORE — a real finding: the capstone line is unconditional, even when the
// four failure paths above it set g_bootHookFailed and never early-return
Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] All hooks installed");

// AFTER — the capstone reflects what the failure paths above it actually recorded
Log(EchoVR::LogLevel::Info,
    "[NEVR.PATCH] boot hooks: %d installed, %d failed%s", installedCount, failedCount,
    failedCount > 0 ? " — see WARNING lines above for which" : "");
```

Note the AFTER example is *also* an instance of the general fix for
unconditional summary lines: a summary line's truth value must be computed
from the same state the detail lines above it observed, not asserted
independently.

### Category K: Explain in the line; cite the ticket as a footnote

A ticket reference standing in for an explanation forces the reader to go
find and read the ticket to understand a line in front of them right now.
The explanation belongs in the line. The ticket ref, if kept at all, is a
footnote.

```cpp
// BEFORE — this exact line already exists in this document, in Rule 12's own
// example above; it violates the category the rule it illustrates is not about
[NEVR.PATCH] boot complete: 14 hooks installed, 1 deferred, 1 known-failed (N126/N128), 0 unexpected

// AFTER
[NEVR.PATCH] boot complete: 14 hooks installed, 1 deferred, 1 known-failed
  (target address changed in a prior game update — see N126/N128 for history), 0 unexpected
```

(Rule 12's example above this section should be updated to match the AFTER
form in the same commit that adds this section — it is the one place in this
document that models the anti-pattern it's supposed to prevent.)

### Category L: The sentence has to parse

Read the line as an English sentence, not as a template with blanks filled
in. If a word is missing, it doesn't matter how correct the data is.

```cpp
// BEFORE — a real finding; "behind the game's" has no object
Log(EchoVR::LogLevel::Info,
    "[NEVR.PATCH] Console ctrl handler installed (behind the game's until re-armed)");

// AFTER
Log(EchoVR::LogLevel::Info,
    "[NEVR.PATCH] console ctrl handler installed (behind the game's handler in the chain until re-armed)");
```

### Category M: One failure class, one standard of explanation

The same underlying failure code or condition — `MH_ERROR_ALREADY_CREATED`,
a specific `GetLastError()` value, a parse-error path — hit at more than one
call site SHALL be explained to the same standard everywhere it's caught.
When one site says "benign — a sibling patch already owns this address" and
another site hits the identical code with a bare "hook failed," that's not
two findings, it's one finding with an uneven fix. Search for the failure
constant repo-wide before writing the fix; bring every site up to the best
existing explanation, don't write a new one from scratch at each site.

```cpp
// BEFORE — two sites, same MH_STATUS, two different amounts of information
// site A (mode_patches.cpp): explains it
Log(EchoVR::LogLevel::Warning,
    "[NEVR.PATCH] hook already created at 0x%llX — a sibling patch owns this address, one hook wins, benign",
    va);
// site B (binary_bug_fixes.cpp): doesn't
Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] MH_CreateHook failed");

// AFTER — site B brought up to site A's standard
Log(EchoVR::LogLevel::Warning,
    "[NEVR.PATCH] hook create failed va=0x%llX status=%s%s", va, MH_StatusToString(status),
    status == MH_ERROR_ALREADY_CREATED ? " (a sibling patch owns this address, one hook wins, benign)" : "");
```

---

## References

- **N15** — Login XPID not logged at injection time.
- **N18** — log filter not suppressing noise effectively.
- **N89** — the `log_filter.dll` *plugin* is superseded by the built-in
  filter and is refused by the loader. `src/runtime/log/builtin_filter.cpp`
  is the shipping path; do not reintroduce the plugin.
- **N19** — No logging standards exist (this document).
- **2026-09 message-content audit** — repo-wide review of all 706 `Log()`/
  `FatalError()`/`ServerFatal()` call sites in `src/`, `src/modules/`, and
  `plugins/` against Categories G-M above (N-ledger closed; findings tracked
  as GitHub issues, not N-entries). Basis for the "Message Content Quality"
  section.
- **N17** — Startup hook errors not systematically tracked.
- **AGENTS.md** — Project conventions, `Log()` usage, subsystem architecture.
- **CPP-MINGW-ADDENDUM-GENERIC.md** — "Logging (Structured, Always)" section, "No printf" rule.
- **`src/core/logging.h`** — `Log()` and `FatalError()` declarations.
- **`src/core/logging.cpp`** — `Log()` implementation, `FormatJsonLogEntry`.

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
- You **shall not** log at INFO level inside a per-frame or per-tick hot path. Use DEBUG
  or (better) a rate-limited counter summary.
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
- You **shall not** leave a `// TODO: add logging` comment without a BUGS.md N-ledger
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
| INFO    | Normal state transitions. Startup, shutdown, connected, disconnected,       | Always on  |
|         | registered, session created, login success, config loaded, tick rate set.   |            |
| DEBUG   | Per-request detail, on-disk state, pointer values, hex dumps,               | **Off**    |
|         | per-frame counters, values that aid diagnosis but are not needed for        |            |
|         | operational monitoring. Rate-limit anything in a hot path.                  |            |

**Anti-patterns by level:**

- **ERROR used for routine failures:** A login rejection with a
  well-defined error code is WARNING, not ERROR. ERROR means "the
  process may not be able to continue." A retryable network timeout is
  WARNING, not ERROR.
- **INFO used for per-frame data:** Telemetry diagnostics, per-frame
  counters, hex dumps of game state — these are DEBUG. They drown
  operational signal at INFO level.
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
violations are recorded in BUGS.md N19 (no logging standards exist).

### Rule 2: XPID shall be logged at login

The login/acquisition path SHALL log the full platform-identity string
being used. The XPID is the external identifier that ties a log session
to a specific user account — it is the single most important identifier
in the log.

```cpp
// BEFORE (N15 — numeric account ID only, no platform prefix, no full XPID)
// was: src/modules/ws-bridge/src/ws_bridge.cpp:281-283 — directory deleted in
// 2e5b4ec; retrieve with
// git show d654cd192e95767227fda0313d713e9d5effe4c9:src/modules/ws-bridge/src/ws_bridge.cpp
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
login payload, not hardcoded. If the platform is DSC (Discord, code 2),
the XPID is `DSC-<id>`, not `OVR-ORG-<id>`. See BUGS.md N14 (platform
prefix hardcoded as OVR_ORG in module ws_bridge).

**Where:** the module copy is GONE — `src/modules/ws-bridge/` was deleted in
`2e5b4ec` (N105) after N92 folded the bridge into `BugSplat64.dll`. Its content
is still retrievable:
`git show d654cd192e95767227fda0313d713e9d5effe4c9:src/modules/ws-bridge/src/ws_bridge.cpp`
(conn>0 injection at :281-283, conn=0 at :441-444). The surviving injection site
is `src/runtime/compat/ws_bridge.cpp:515`. Tracked as BUGS.md N15.

### Rule 3: Silence is not success

A subsystem that produces no log lines is indistinguishable from one
that never ran. Every significant state transition SHALL produce a log
line at the point of transition.

The following transitions are **mandatory** INFO-level log points:

- **Startup:** component initialized, hooks installed, config loaded,
  modules/plugins loaded (each with name and version).
- **Connectivity:** WebSocket connected (with URI and connection index),
  WebSocket disconnected (with code and reason), WebSocket error (with
  error string).
- **Registration:** registration request sent (with message type and
  size), registration success received (with server_id),
  registration failure received (with error code and message).
- **Login:** login request injected (with XPID, connection index, and
  size), login success received, login failure received (with status
  code and error message).
- **Session:** session created, session started, session ended (each
  with session ID).
- **Shutdown:** component shutting down, hooks removed, connections
  closed.

A component that initializes silently is a component whose failure is
undetectable. Every `Init()` function SHALL log at entry and exit, with
the exit log including success/failure and any relevant state.

### Rule 4: Noise is a defect

echovr-native log lines are "objectively 97% worthless" (owner). The
built-in log filter exists to suppress them (see BUGS.md N18). If noise is
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
- **Per-frame or per-tick diagnostics at INFO level.** These are DEBUG.
  If they appear at INFO level, that is a bug.
- **Hex dumps, pointer values, and raw binary at INFO level.** These
  are DEBUG, gated by a verbosity flag.

**Filter audit checklist (N18 fix direction):**
1. Verify the built-in log filter is capturing game lines (its health line
   reports `game_lines=`; zero means another module has taken the hook — N89)
   configuration.
2. Capture a representative server log from a live session.
3. Count lines per subsystem tag; any tag with >50% of total lines is a
   candidate for rate-limiting or DEBUG demotion.
4. Identify any game-native line (no `[NEVR.*]` prefix) appearing more
   than once per 10 seconds — add a suppression rule.
5. After tuning, re-capture and verify that >80% of lines carry a NEVR
   subsystem tag and a traceable identifier.

### Rule 5: Hook failures are WARNINGS, not silent drops

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
`src/runtime/log/builtin_filter.cpp:826`. Tracked as BUGS.md N17
(startup hook errors not systematically tracked).

### Rule 6: The Log() function is the single entry point

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

`docs/reference/logging-format.md` documented the `FormatJsonLogEntry` shape as
though it were the live output. It was removed 2026-07-29 rather than corrected,
since it described a format that has not shipped since March and contradicted this
section. Retrieve it with:

    git show 9bf274450e2ddbcbba5f61dc67f23f14f5c3e064:docs/reference/logging-format.md

### Rule 7: State transitions log FROM -> TO

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

### Rule 8: Connection-scoped events carry the connection index

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

### Rule 9: Error paths log the error code

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

### Rule 10: Config values logged at load time

Every config value that affects runtime behavior SHALL be logged at
INFO level when it is loaded. This includes:

- CLI flags (`-server`, `-headless`, `-timestep`, `-telemetry`).
- Config-file overrides (arena round time, mercy score, service URLs).
- Service redirects (every URL that is redirected to the bridge).
- Module/plugin load decisions (loaded, skipped, failed).

The log is the only record of what configuration the process is running
with. If a config value is not logged, the operator must guess whether
it was applied.

---

## BEFORE / AFTER Reference

These examples are drawn from actual code in the repository. The BEFORE
column shows the current log line; the AFTER column shows the corrected
form.

| Context | BEFORE | AFTER |
| ------- | ------ | ----- |
| Login injection (N15) | `"[NEVR.WS] Injected LoginRequest (OVR-ORG-%llu, %zu bytes)"` | `"[NEVR.WS] login injected xpid=%s platform=%d conn=%d size=%zu"` |
| WebSocket connected | `"[WEBSOCKET] Connected to ServerDB"` | `"[NEVR.WS] websocket connected uri=%s conn=%d"` |
| WebSocket disconnected | `"[WEBSOCKET] Disconnected from ServerDB (code: %d, reason: %s)"` | `"[NEVR.WS] websocket closed conn=%d code=%d reason=%s"` |
| Login success | `"[NEVR.WS] LOGIN SUCCESS"` | `"[NEVR.WS] login success xpid=%s conn=%d session=%s"` |
| Login failure | `"[NEVR.WS] LOGIN FAILURE: status=%llu msg=%.*s"` | `"[NEVR.WS] login failed xpid=%s conn=%d status=%llu reason=%s"` |
| Hook failure | `"[wave0] FAILED to hook fcn.0x%llX"` | `"[NEVR.PATCH] hook failed name=%s va=0x%llX expected=%s actual=%s"` |
| Config redirect | `"[NEVR.PATCH] Service redirect [%s]: %s -> %s"` | `"[NEVR.PATCH] service redirect key=%s from=%s to=%s"` |
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
| WebSocket disconnect     | INFO  | Once             | Log code + reason + conn index              |
| WebSocket error          | WARN  | Once per failure | Log error string + conn index               |
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
| INFO in per-frame hot path                   | Floods the log                    | Demote to DEBUG or rate-limit                         |
| printf/fprintf/cerr instead of Log()         | Bypasses structured logging       | Use Log() from logging.h                              |
| Game-native line without NEVR annotation     | Noise (N18)                       | Suppress or wrap with structured fields               |
| Free-text message with no key=value fields   | Not machine-parseable             | Use key=value format for identifiers and outcomes     |
| Config value not logged at load              | Configuration is invisible        | Log at INFO with key + value                          |

---

## References

- **BUGS.md N15** — Login XPID not logged at injection time.
- **BUGS.md N18** — log filter not suppressing noise effectively.
- **BUGS.md N89** — the `log_filter.dll` *plugin* is superseded by the built-in
  filter and is refused by the loader. `src/runtime/log/builtin_filter.cpp`
  is the shipping path; do not reintroduce the plugin.
- **BUGS.md N19** — No logging standards exist (this document).
- **BUGS.md N17** — Startup hook errors not systematically tracked.
- **BUGS.md N14** — Platform prefix hardcoded as OVR_ORG (affects XPID correctness).
- **AGENTS.md** — Project conventions, `Log()` usage, subsystem architecture.
- **CPP-MINGW-ADDENDUM-GENERIC.md** — "Logging (Structured, Always)" section, "No printf" rule.
- **`src/core/logging.h`** — `Log()` and `FatalError()` declarations.
- **`src/core/logging.cpp`** — `Log()` implementation, `FormatJsonLogEntry`.

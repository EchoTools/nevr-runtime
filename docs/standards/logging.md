# nEVR Runtime Logging Standards

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED",
"NOT RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be interpreted as described in BCP 14
([RFC 2119](https://www.rfc-editor.org/rfc/rfc2119), [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174)) when, and only
when, they appear in all capitals, as shown here.

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

- A binding ruleset for every `Log()` call in `src/`, `sdk/`,
  `modules/`, and any future component that links `libcommon.a`.
- A review gate: the "Hard Stops" table at the end of this document is
  enforced. A log line that fails any check is rejected in review.
- A definition of what constitutes noise and what a log line
  SHALL carry to be actionable.
- The single authority on log level usage in this project. If you are
  unsure whether something is INFO or DEBUG, the answer is here.

### This is NOT

- A tutorial on the `Log()` function signature. That is in
  `src/core/logging.h` and `AGENTS.md`. This document defines WHAT
  a record carries (its tag, event, fields and message), not how to call
  the function. The record's shape and the structured call are ADR 0027
  (`docs/adr/0027-structured-logging.md`).
- A style preference. The "Hard Stops" are not negotiable; they are the
  mechanical difference between a log that diagnoses a problem and one
  that wastes the operator's time.
- Optional for "debug prints" or "temporary" log lines. Every log line
  in every `.cpp` and `.h` file is in scope. If it ships, it must meet
  the standard.

### You SHALL

- You **SHALL** use `Log()` as the single entry point. Its structured form,
  `Log(level, tag, event, {fields})`, is specified by ADR 0027 and is not built yet; until it
  is, and for a call site the migration has not reached, the entry point is the printf form
  `Log(EchoVR::LogLevel::level, "format", ...)`. No `printf`, no `fprintf`, no `cerr`, no
  `OutputDebugString`, no `std::cout`. See `src/core/logging.h` and the C++ Mingw Addendum in `AGENTS.md`.
  A plugin is a separate DLL and cannot call `Log()`; its single entry point is the host's
  log function in its context, reached through `plugin_logger.h` (Rule 15).
- You **SHALL** include a subsystem tag on EVERY log line. The tag identifies which
  nEVR component produced the line. See the Subsystem Tags table below.
- You **SHALL** log the outcome. A log line that says "connecting" without a
  corresponding "connected" or "connection failed" is incomplete.
- You **SHALL** log the relevant identifier. Connection index, session ID, XPID
  string, server ID, or request ID — there is always one identifier
  that the operator needs to trace the event through the system.
- You **SHALL** use the correct log level per the Level Guidelines below. When in
  doubt, go one level higher (DEBUG -> INFO, INFO -> WARNING) — a
  too-verbose log can be filtered; a missing log cannot be recovered.
- You **SHALL** log identity at login. Every login/acquisition path SHALL include the
  full platform-identity string (XPID) as a structured field (see Rule 2).

### You SHALL NOT

- You **SHALL NOT** log a bare free-text message with no subsystem tag and no identifier.
  `Log(Info, "Connected")` is not a log line — it is a riddle.
- You **SHALL NOT** log at INFO level inside a hot path (per-frame, per-tick,
  per-connection). Use DEBUG for the narrative detail; emit one INFO
  summary line instead (Rule 12).
- You **SHALL NOT** log a state transition without also logging the state it transitioned
  FROM and TO. "State changed" without the old and new states is
  useless.
- You **SHALL NOT** log an error without the error code. `"Failed to connect"` without
  the `WSAGetLastError` or HRESULT is not actionable.
- You **SHALL NOT** suppress a hook installation failure silently. Hook failures are
  WARNING-level events with the target address, expected prologue, and
  actual bytes (see Rule 5).
- You **SHALL NOT** log raw binary data, hex dumps, or pointer values at INFO level.
  These are DEBUG-level diagnostics and SHALL be gated by a verbosity
  flag or compile-time guard.
- You **SHALL NOT** use `Log(Debug, ...)` for anything you expect an operator to need
  during a production incident. DEBUG is off by default in production
  builds. If an operator needs it, it is INFO.
- You **SHALL NOT** leave a `// TODO: add logging` comment without a N-ledger
  entry. A TODO without a ticket is a wish.

---

## Subsystem Tags

Every record carries a tag identifying the emitting component: the value
of its `tag` key, written without brackets (`NEVR.WS`), and shown in
brackets at the start of its console line and of a printf call's text.
Tags are hierarchical: `[NEVR.COMPONENT]` for nEVR-authored
code, `[COMPONENT]` (no NEVR prefix) for third-party or game-native
subsystems that nEVR annotates. ADR 0027's `Tag` enum is this table, row
for row.

| Tag                   | Component                                     |
| --------------------- | --------------------------------------------- |
| `[NEVR.WS]`           | ws-bridge (WebSocket proxy, login injection)  |
| `[NEVR.GAMESERVER]`   | GameServerLib (lobby registration, sessions)  |
| `[NEVR.PATCH]`        | gamepatches (boot hooks, config, CLI, mode)   |
| `[NEVR.HEADLESS]`     | Headless graphics stub, render-skip patches   |
| `[NEVR.MODULE]`       | Module registry and static modules            |
| `[NEVR.PLUGIN]`       | Plugin loader (discovery, lifecycle) and the lines plugins log through the host (`plugin=<id>`, Rule 15) |
| `[NEVR.TELEMETRY]`    | Telemetry streamer (WebSocket, snapshots)     |
| `[NEVR.XPID]`         | Platform-identity patches (DSC provider)      |
| `[NEVR.CONFIG]`       | Config loading, service redirects             |
| `[NEVR.STOP]`         | A command line the run is refused for: the stop line (JSON), exit code 64 |
| `[NEVR.CRASH]`        | Crash recovery, dump, longjmp, and the readable crash report: one `crash: <signal> <module>+<offset> <symbol> report=<path>` line per report (`docs/reference/crash-report.md`) |
| `[NEVR.AUTH]`         | Token acquisition, device-code flow, refresh  |
| `[NEVR.CDN]`          | Asset CDN download and override              |
| `[NEVR.UPNP]`         | UPnP port mapping                            |
| `[NEVR.RESOURCE]`     | Resource override / embedded asset injection |
| `[NEVR.LOGFILTER]`    | The log filter's own health and rate summary |
| `[NEVR.DLLHOOK]`      | LoadLibrary interception                     |
| `[NEVR.PROFILE]`      | Server profile / memory snapshot             |
| `[NEVR.PERF]`         | Frame-time probe (`diagnostics.frame_stats`): the start line and one summary line per 10 s window |
| `[NEVR.FATAL]`        | Fatal-error path (ServerFatal)               |
| `[NEVR.SCENARIO]`     | Scenario-test control endpoint (test builds only; never in a release DLL) |
| `[NEVR.SELFCHECK]`    | Self-checks: one result per run-card check (every build, `compat/self_check.h`) |
| `[NEVR.MENU]`         | Menu text the runtime sets: the nEVR version on the menu's versions panel (`patch/menu_version_label.h`) |
| `[NEVR.SPLASH]`       | A button press skips the splash screen: the Continue element going live, each skip with the button id and how long the button was live (`patch/splash_skip.h`) |
| `[NEVR.UITEXTURE]`    | UI, splash and menu textures at their top mip: one line per UI texture naming the mip requested (`patch/ui_texture_mips.h`) |
| `[NEVR.UI]`           | The UI text writer (`patch/ui_text.h`, `patch/ui_text_writer.h`, #896): `text writer ready`/`off` with the three pins, one `text written` or `text dropped` line per request with `actor`, `element`, `bytes` and `attempts`, and `text first write` with the writing thread and the game-loop thread. A text is never logged, only its size |
| `[NEVR.INVITE]`       | Invite codes for the CREATE GAME / JOIN VIA CODE wizard (`patch/invite_wizard.h`, ADR 0017): a code is never logged, only `code_len` and `code_fp` |
| `[NEVR.RENDER]`       | The render configuration of a PC client: one `render_config` line at the first rendered frame and one for every change, with the GPU and the rating score the auto-selector reads (`gpu_score`), headset, refresh rate, render target and the game's graphics settings, plus the hooks' own install lines, a one-time `settings_trace` line, and one `gpu_score kept|filled|unrated` line a run, which says the score the selector read and, when unrated, why (`patch/render_config.h`, `patch/render_config_hook.h`, `patch/gpu_score.h`, ADR 0022, #1419) |
| `[NEVR.HOST]`         | The `BugSplat64.dll` host's own JSONL records (`nevr-bootstrap.jsonl`): attach decisions, runtime load, crash dump, crash report send (`event` `crash_send`, #1689), and the console notice that follows a `game_stop` (`event` `console_notice`, #1533: `reason`, and `written`, whether a console took the plain line). The value of the record's `tag` field; the host writes no `Log()` line |
| `[NEVR.STOP]`         | The one line the `BugSplat64.dll` host writes to standard error when it stops the game or leaves it running without `nevr-runtime.dll` (`runtime/host/stop_line.h`, #1252), with the same facts in the host log. The value of the line's `tag` field; fields and exit codes in "The `NEVR.STOP` line" below |
| `[NEVR.LOGSEND]`      | The player log upload (`core/log_send.h`, ADR 0030, #1788): one Info line per tap with the file names and sizes, one Info line with the status and the code, a Warning with the status and the transport error or HTTP status on a failure. Never the text of a log, the body or the answer |
| `[NEVR.INSTALL]`      | The core installer's own JSONL run log (`installer/`, ADR 0023): one record per event, appended, never truncated; the value of the record's `tag` field. Never carries a token or a response body |
| `[server_timing]`     | Server tick-rate / timing patches             |

**Rule:** If you add a new component, add its tag to this table. If you
are unsure which tag to use, use the nearest existing tag and note it
for review.

### The `NEVR.STOP` line

Every way the host stops the game, and every attach that leaves the game running without the runtime, writes one JSON
object on one line to standard error, once per run. A `-server` operator has a console and no dialog; this is what they
read. The host log (`nevr-bootstrap.jsonl`) carries the same facts: in the `game_stop` record when the host ends the
process, and in a `host_not_attached` record when the game keeps running. The line is built in a fixed buffer
(`JsonLine`, no heap, no lock) and written with one `WriteFile`. The exit codes do not change.

| Field             | Meaning                                                                                              |
| ----------------- | ---------------------------------------------------------------------------------------------------- |
| `tag`             | `NEVR.STOP`                                                                                          |
| `ts`, `pid`       | UTC millisecond stamp and process id, as in every host record                                        |
| `event`           | `stop`                                                                                               |
| `stopped`         | `true` when the host ends the process; `false` when the game keeps running without the runtime       |
| `code`            | The exit code, as a number. Absent when `stopped` is `false`                                         |
| `reason`          | One token from the table below                                                                       |
| `host_version`    | The version in the host's own `NEVR-BUILD <version> <commit40>` literal                              |
| `runtime_version` | The version in `nevr-runtime.dll`'s literal when the host read the file, else `not loaded`           |
| `detail`          | What is wrong and where, in words: the path searched, the verifier's own words, the Win32 error or the ABI status. When the runtime is absent from the nEVR root and a `nevr-runtime.dll` sits beside `BugSplat64.dll`, it names that folder, says the host does not load that file and names the root to move it to. A piece that does not fit the detail's buffer is left out whole and the detail ends with `[detail incomplete: a path did not fit]` |
| `log`             | The full path of the host log file that accepted the record (the %TEMP% copy when the primary folder refused it); the record and the line name the same file. Empty when no log file did |

| `reason`                 | `stopped` | `code`                    | Meaning                                                                         |
| ------------------------ | --------- | ------------------------- | ------------------------------------------------------------------------------- |
| `runtime_absent`         | `true`    | 78                        | No `nevr-runtime.dll` in the nEVR root                                          |
| `runtime_refused`        | `true`    | 78                        | The signature check refused the file, or it could not be read; it is never loaded |
| `dependency_load_failed` | `true`    | 78                        | A DLL that `nevr-runtime.dll` imports could not be loaded                       |
| `load_failed`            | `true`    | 78                        | `LoadLibrary` failed for another cause                                          |
| `missing_entry`          | `true`    | 78                        | The file has no `NEVR_GetRuntimeApi` export                                     |
| `null_descriptor`        | `true`    | 78                        | `NEVR_GetRuntimeApi` returned no descriptor                                     |
| `abi_rejected`           | `true`    | 78                        | The descriptor's ABI version or size was rejected                               |
| `init_failed`            | `true`    | 78                        | `initialize` returned non-zero or threw                                         |
| `no_path`                | `true`    | 78                        | No nEVR root could be formed (no `%LOCALAPPDATA%` that fits)                    |
| `unsupported_image`      | `true`    | 3762421763 (`0xE0420003`) | The process image is not the supported `echovr.exe` build (the image guard)     |
| `host_not_attached`      | `false`   | none                      | The image check, the prologue check or the hook install failed: no detour was installed and the game runs without the runtime. `detail` names the step |

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

The printf examples in these rules show what a record carries, in the form
the code uses today. Each `key=value` token in them is a field, which the
structured call (ADR 0027) writes as its own JSON key with its own type,
and the leading tag is the record's `tag` key.

### Rule 1: Every log line is a structured claim

Every record carries four things. A record missing any of these is
**naked** and is noise.

1. **WHAT** — the event that occurred (connected, disconnected, failed,
   registered, injected, redirected, loaded, crashed): the record's `event`.
2. **WHERE** — the subsystem tag (see Subsystem Tags table): the record's `tag`.
3. **IDENTIFIER** — the traceable entity (connection index, session ID,
   XPID, server ID, request ID, module name, URI): a field.
4. **OUTCOME** — the result (success/error code, bytes transferred, old
   state -> new state, duration, count): a field.

```cpp
// BEFORE (naked — no identifier, no outcome)
Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Connected to server");

// AFTER (carries WHAT=connected, WHERE=[NEVR.GAMESERVER], ID=uri, OUTCOME=success)
Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] websocket connected uri=%s conn_id=%d",
    uri, connIndex);

// AFTER, structured (ADR 0027): the event, the tag and each field are JSON keys
Log(Level::Info, Tag::GameServer, "websocket_connected",
    {Str("uri", RedactUrlForDiagnostics(uri)), Int("conn_id", connIndex)});
```

**Where:** This rule applies to every `Log()` call site.

### Rule 2: XPID SHALL be logged at login

The login/acquisition path SHALL log the full platform-identity string
being used. The XPID is the external identifier that ties a log session
to a specific user account — it is the single most important identifier
in the log.

```cpp
// BEFORE — numeric account ID only, no platform prefix or full XPID
Log(EchoVR::LogLevel::Info,
    "[NEVR.WS] Injected LoginRequest (OVR-ORG-%llu, %zu bytes)",
    (unsigned long long)discordId, loginMsg.size());

// AFTER — full XPID string, connection index, byte count
std::string xpid = platformPrefix + "-" + std::to_string(accountId);
Log(EchoVR::LogLevel::Info,
    "[NEVR.WS] login injected xpid=%s platform=%d conn=%d (%s) size=%zu",
    xpid.c_str(), platformCode, connIdx, ConnLabel(connIdx), loginMsg.size());
```

The platform prefix SHALL be derived from the actual platform code in the
login payload, not hardcoded. Platform codes are the game's own
1-indexed numbering (STM=1, DSC=2, XBX=3, OVR_ORG=4, OVR=5, BOT=6, DMO=7);
code 2 is "PSN" in the game's string table and reads "DSC" after the runtime rewrites it.
The bridge logs in as OVR_ORG (code 4), so its XPID is `OVR-ORG-<id>`, except
when the game marks the client No VR (bit 20 of the game's flag word, which
the stock login also reads to send the "N/A" HMD serial): that client logs in
as DMO (code 7) and its XPID is `DMO-<id>`. A login as DSC (code 2) would
produce `DSC-<id>`. `login platform=<code> (<prefix>) no_vr=<0|1> game_flags=0x<hex>`
precedes the `login injected` line and records the flag word the choice was
made from.

**Where:** the injection site is `InstallWebSocketBridge` in
`src/runtime/compat/ws_bridge.cpp` (the `login injected xpid=` log line).

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

echovr-native log lines are noisy. The built-in log filter exists to suppress
them. If noise is
reaching the production log, the filter is broken and that is a defect.

What constitutes noise:

- **Repeated identical lines.** A log line that appears more than once
  per second with identical content is noise. Use rate-limited summary
  logging instead ("X repeated N times in the last T seconds").
- **Lines with no structured fields.** A log line that carries only a
  free-text message with no identifier, no subsystem tag, and no outcome
  is noise. See Rule 1.
- **Game-native lines that nEVR doesn't annotate.** Lines from
  `echovr.exe` that pass through unmodified, without an nEVR subsystem
  tag or structured wrapper, are noise. The log_filter SHALL suppress
  these in production server builds.
- **Per-item detail at INFO level.** Per-frame, per-tick, or per-connection
  diagnostics at INFO are DEBUG. INFO gets the summary; DEBUG gets the
  narrative (Rule 12).
- **Hex dumps, pointer values, and raw binary at INFO level.** These
  are DEBUG, gated by a verbosity flag.

**Filter audit checklist:**
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

**Where:** `src/runtime/patch/binary_bug_fixes.cpp`,
`src/runtime/patch/headless_graphics.cpp`,
`src/runtime/patch/resource_override.cpp`, and
`src/runtime/log/builtin_filter.cpp`.

### Rule 7: The Log() function is the single entry point

`Log()` in `src/core/logging.h` is the mechanism: the structured form of
ADR 0027, and the printf form `Log(EchoVR::LogLevel::level, "format %d", val)`
for a call site not yet migrated. This standard defines what a record
carries and what level to use. No other output mechanism is permitted.

```cpp
// Good — uses Log(), carries all four fields
Log(EchoVR::LogLevel::Info,
    "[NEVR.GAMESERVER] registration response server_id=%llu ip=%s",
    (unsigned long long)serverId, ipAddress.c_str());

// Good — the same record, structured (ADR 0027); a 64-bit id is a string
Log(Level::Info, Tag::GameServer, "registration_response",
    {Id("server_id", serverId), Str("ip", ipAddress)});

// Good — uses FatalError for unrecoverable termination
FatalError("Required module missing", "ws_bridge");

// Bad — never do this
printf("registration response server_id=%llu\n", serverId);
fprintf(stderr, "error: %s\n", what());
OutputDebugStringA("got here");
std::cerr << "failed" << std::endl;
```

### A record's fields are JSON keys (ADR 0027)

A record SHALL be one JSON object on one line: the header keys `schema`, `ts`,
`run` (N80), `level`, `tag` and `event`, an optional `msg`, then each field as
its own key with its own JSON type. A field SHALL NOT be written as text, as
`key=value` text or as JSON inside `msg`; `msg` is a literal sentence that
states the consequence (Category G), never data. The run log is the one file
stream (the built-in filter, `src/runtime/log/builtin_filter.cpp`); the console
line is a rendering of the record, not a second log.

The reason: a record whose subsystem, event and identifiers are prose inside
`msg` can be read only by a regex, and its values have no types. journald's
field list says its `MESSAGE=` "is not supposed to be parsed for metadata", and
the OpenTelemetry log data model keeps a record's Body apart from its
Attributes. ADR 0027 gives the schema, the call, the console rendering, the
migration of every `Log()` call and the prior art.

Today the run log's records (`WriteFileRecord`) carry `ts`, `run` and `level`
and put the whole line in `msg`, and our lines reach that file through the
game's logger (`EchoVR::WriteLog`, captured by the filter's `CLog::PrintfImpl`
hook). ADR 0027's first stage adds the header keys and writes our records
without the game's logger; each migration then moves a call's fields out of
`msg`.

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
index in the ws-bridge (0 = a config socket, 1 = the login socket, 2 and up =
the sockets that share the login session). The index is the slot the socket
took, decided by the request target it asked for (`/config`, `/login`), not by
the order it connected in; the `game socket admitted` line records the target,
the role and the basis for each socket. Without the index, an operator cannot
correlate the connect, message, and disconnect events for a single connection.

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

The runtime boots under the DllMain loader lock, where the game's CLog is not
usable. The boot phase runs from `nevr_boot_log_tee::Init()` to
`nevr_boot_log_tee::Close()`, and in it `Log()` itself reports through the boot
tee (stderr and `nevr-boot.jsonl`): `Init()` sets `Log()`'s boot sink
(`SetBootLogSink` in `src/core/logging.h`) and `Close()` clears it. Code that
runs both during boot and after it calls `Log()` and never branches on the
phase.

`Log()` in the boot phase puts the level after the subsystem tag
(`[NEVR.PATCH] warn; hook FAILED ...`). A direct
`nevr_boot_log_tee::TeeFprintf` call, for code that runs only in the boot
phase, SHALL embed its level in the format string the same way, so boot lines
are distinguishable from timestamped `Log()` output:

```cpp
// BEFORE (no level — indistinguishable from any other bootstrap line)
nevr_boot_log_tee::TeeFprintf("[NEVR.BOOT] installing crash recovery hooks...\n");

// AFTER
nevr_boot_log_tee::TeeFprintf("[NEVR.BOOT] info; installing crash recovery hooks...\n");
```

The level prefix (`debug;`, `info;`, `warn;`, `error;`) bridges the gap
until the game logger is usable. After the boot phase, all output SHALL use
`Log()`: `TeeFprintf` is a bootstrap mechanism only. A structured record
(ADR 0027) carries its level in its `level` key and has no prefix; the prefix
belongs to printf text only, and goes with it.

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
  (optional target unavailable; patch skipped), 0 unexpected

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

### Rule 15: Plugins log through the host

A plugin's lines go to the host's `Log()` through `ctx->log` (`NevrLogFn` in
`sdk/include/nevr/plugin_interface.h`, API v6), so they reach the same file, the same filter and the
same level control as the host's own lines. A plugin does not write to stderr, which the host does not
collect, and does not open its own log file. `sdk/include/nevr/plugin_logger.h` is the way to
call it (`PluginLogBind(ctx)` in init, then `PluginLog`, `PluginLogWarning`, `PluginLogError`,
`PluginLogDebug` or `PluginLogFields`).

- **Before the bind.** A line logged before `PluginLogBind(ctx)` is held in memory and written through the host when
  the bind succeeds, in the order it was logged (`plugin_logger.h`). Only a bind that finds no host log writes the held
  lines to stderr.
- **Tag and identity.** The host writes `[NEVR.PLUGIN] plugin=<id> <message> <key>=<value> ...`. `<id>`
  is the host's name for the plugin (the `name` of its `config.yaml` entry), not text the plugin
  chooses, so one plugin cannot write as another and an operator can filter on `plugin=`.
- **In the file.** Today that line is one `Log()` text. Under ADR 0027 the record carries `tag`
  `NEVR.PLUGIN`, `plugin` (the id, set by the host), `event` `plugin_line`, the message as `msg` and
  each field as its own key with a string value; the console line stays as shown above, and the
  plugin ABI does not change.
- **Level.** The plugin names one of `NEVR_LOG_DEBUG`, `NEVR_LOG_INFO`, `NEVR_LOG_WARNING`,
  `NEVR_LOG_ERROR`; the Level Guidelines apply to a plugin as they do to the host. A level outside the
  range is logged as a warning that says so (`level_invalid=<n>`).
- **Fields.** Identifiers and outcomes go in key/value fields (Rule 1), not in the message text. A
  value with a space, `=` or a quote is quoted; a key is `[A-Za-z0-9_.-]`.
- **Message is text.** It is never a format string on the host side. Control characters become
  spaces, so a plugin cannot start a second line, and message, keys and values are capped so a line
  fits the 2048-byte buffer `Log()` uses before the game logger exists; fields past the cap are counted
  in `fields_dropped=<n>`.
- **When.** A plugin has a host log from its init callback (`NvrPluginInit` or `NvrPluginInitEx`)
  on, from any thread, including from `NvrPluginShutdown` with the id and function it kept. There is
  none before init (no context exists) and none from `DllMain`, which runs under the loader lock. A
  host older than v6 passes a context that does not cover `ctx->log`; the plugin logger then falls back
  to stderr (`<prefix> <LEVEL> <message>`), which that host does not collect.
- **Not provided.** A per-plugin log file, and rate limiting or log-once helpers. A plugin that logs
  per frame keeps its own cap (Rule 5, Rule 13).

**Where:** `src/runtime/ext/plugin_loader.cpp` (`HostPluginLog`), `src/runtime/ext/plugin_log.h`.

### Rule 16: The bridge records the lobby requests the client sends and the answers it gets

The bridge writes one Info record for each lobby message that decides where the player ends up, on a client or a server (the PC bridge writes
all four lines; the Quest router writes the two session lines), so a tester's log answers "how many join attempts did this client make, and what did each get". All four lines carry
the tag `[NEVR.WS]` and the connection index (Rule 9):

```
[NEVR.WS] lobby find requested conn=2 mode=<name or 0x..> entrants=3 accounts=OVR-ORG-1,OVR-ORG-2,OVR-ORG-3 current_lobby=<GUID or none> group=<GUID>
[NEVR.WS] lobby join requested conn=3 lobby=<GUID> entrants=1 accounts=OVR-ORG-1
[NEVR.WS] lobby session success conn=3 lobby=<GUID> group=<GUID> party_id=<n|none> party_size=<n> party_leader=<yes|no|unknown> last_request=<find|join|none> last_request_age_ms=<n|none> request_outstanding=<yes|no>
[NEVR.WS] lobby session failure conn=3 code=8 reason=lobby_full group=<GUID> message_bytes=<n> party_id=<n|none> party_size=<n> party_leader=<yes|no|unknown> last_request=<find|join|none> last_request_age_ms=<n|none> request_outstanding=<yes|no>
```

- **Ids, not names.** A lobby or group is its GUID in upper case and the Windows layout, the text the service
  writes as the party data's `lobbyid`; an entrant is its XPID. `current_lobby=none` is a find from no lobby.
- **The outcome is the code.** `code` is the number in the frame and `reason` is the game's own name for it
  (echovr.exe 0x14017d380, `CR15NetClientLobby::LobbySessionFailureCB`: 0 to 13, `lobby_full` is 8); a number
  outside 0 to 13 reads `unknown`. A failure frame carries no lobby id, so a session record names the request it
  answers through `last_request` (below), not through its `conn`.
- **The party and the request a session record names.** `party_id`, `party_size` and `party_leader` are the party
  state when the record is written: `party_id=none party_size=0 party_leader=no` outside a party; `party_leader` is
  `yes` when the owner is the local account, `no` when another account owns the party, and `unknown` while either
  id is still zero. `last_request` is the last find or join the game sent on this client, on any connection, and
  `last_request_age_ms` is the time from that request to this answer (`0` when the clock went backwards).
  `request_outstanding=yes` only for the first answer to that request, so a second answer reads `no`. With no
  request sent since the process started, the three request fields read `none`, `none` and `no`. An answer whose
  record the rate limit suppresses still takes its request (see Rate), so the next written record is correct.
- **Remote text is not a field** (Rule 4). The settings JSON of a request and the message of a failure are
  never read out. A failure's message is counted instead: `message_bytes=<n>` is the number of its bytes before
  the NUL, at most the field's 64, and `0` when the message is empty or absent.
- **A frame the decode cannot read** is a Warning, `lobby <find request|join request|session success|session
  failure> could not be read conn=<n> payload_bytes=<n>`. Nothing is read past the frame's end.
- **Both run modes.** The records are written on a dedicated server as on a client: the same frames reach
  `ObserveSocialFrames` in `src/runtime/compat/ws_bridge.cpp` in either mode, and the per-message lines of
  `LogFrameMessages` are Debug in both (Rule 13).
- **Rate.** The bridge cannot bound how often the game sends a request: its party-follow loop runs from the
  game's own update. Each kind (find, join, success, failure) writes at most 10 records per 10 s window and
  counts the rest. The next lobby frame of any kind after the window ends writes
  `[NEVR.WS] lobby frame records suppressed kind=<find|join|success|failure> count=<n> window_ms=10000`, with
  the exact number of records the window did not write, so requests per second are `(10 + count) / 10` for a
  full window.
- **Not covered.** The create request and the legacy-family symbols (`compat/legacy_codec.h`). The Quest target
  writes the two `lobby session` records, from the answers its router hands the game (`compat/session_router.cpp`,
  wired in `src/quest/net/session_bridge.cpp`), with the same text. It writes no find or join record, so a request
  is not logged on Quest, and its limit counts only the success and failure kinds.

**Where:** `src/runtime/compat/lobby_frame.h` (decode, text, limiter, request tracker, party fields, the session
record, and `ClientLobbyRecords`, which both writers use); `LogClientLobbyMessage` in
`src/runtime/compat/ws_bridge.cpp` (PC) and `Router::LobbyAnswersLocked` in `src/runtime/compat/session_router.cpp`
(Quest). Tests: `WsBridgeLobbyFrameLog` in `src/runtime/tests/test_behavioral.cpp`; `LobbyFrameLimiter`,
`LobbyFrameRequests`, `LobbyFrameText` and `LobbyFrameRecords` in `src/runtime/tests/test_lobby_frame.cpp`; the
`lobby records` cases in `src/quest/tests/session_router_test.cpp`. Both suites compare their output with the
expected lines in `src/runtime/tests/lobby_record_golden.h`, so the two writers' bytes are pinned to one text.

---

## BEFORE / AFTER Reference

These examples are drawn from actual code in the repository. The BEFORE
column shows the current log line; the AFTER column shows the corrected
form, in the printf form; each `key=value` token in an AFTER is a field the
structured call (ADR 0027) writes as its own key.

| Context | BEFORE | AFTER |
| ------- | ------ | ----- |
| Login injection | `"[NEVR.WS] Injected LoginRequest (OVR-ORG-%llu, %zu bytes)"` | `"[NEVR.WS] login injected xpid=%s platform=%d conn=%d (%s) size=%zu"` |
| WebSocket connected | `"[NEVR.SERVERDB] Connected to ServerDB"` | `"[NEVR.WS] websocket connected uri=%s conn=%d"` |
| WebSocket disconnected | `"[NEVR.SERVERDB] Disconnected from ServerDB (code: %d, reason: %s)"` | `"[NEVR.SERVERDB] Disconnected from ServerDB (code: %u) reconnect_count=%u"` |
| Login success | `"[NEVR.WS] LOGIN SUCCESS"` | `"[NEVR.WS] login success xpid=%s conn=%d session=%s"` |
| Login failure | `"[NEVR.WS] LOGIN FAILURE: status=%llu msg=%.*s"` | `"[NEVR.WS] login failed status=%llu message_bytes=%zu"` |
| Hook failure | `"[wave0] FAILED to hook fcn.0x%llX"` | `"[NEVR.PATCH] hook failed name=%s va=0x%llX expected=%s actual=%s"` |
| Config redirect | `"[NEVR.PATCH] Service redirect [%s]: %s -> %s"` | `"[NEVR.PATCH] service redirect key=%s from=%s to=%s"` (URLs redacted) |
| Module loaded | `"[NEVR.MODULE] Loaded: %s"` | `"[NEVR.MODULE] loaded name=%s path=%s"` |
| Plugin loaded | `"[NEVR.PLUGIN] Loaded: %s v%u.%u.%u (API v%u)"` | Already compliant — carries name, version, API version |
| Plugin line | `fprintf(stderr, "[my_plugin] hook failed %d", rc)` | `PluginLogFields(NEVR_LOG_WARNING, "hook failed", {{"error", "5"}})` -> `"[NEVR.PLUGIN] plugin=my_plugin hook failed error=5"` |
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
| Lobby find/join request, session success/failure (client and server) | INFO | 10 per kind per 10 s, then counted | Rule 16; the count is the suppressed line |
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
| No subsystem tag                             | Can't trace to component          | Give the call its tag (a printf call: the `[NEVR.COMPONENT]` prefix) |
| No identifier                                | Can't correlate events            | Add a field: `conn`, `xpid`, `session_id`, or equivalent |
| No outcome                                   | Can't tell if it worked           | Add success/error code, state transition, or count    |
| XPID not logged at login                     | Can't identify connecting user    | Log the full XPID string                              |
| Hook failure at DEBUG or not logged          | Silent regression in coverage     | Log at WARNING with VA + expected + actual            |
| State transition without FROM state          | Can't diagnose stuck state        | Log old_state -> new_state                            |
| Error without error code                     | Not actionable                    | Add GetLastError(), HRESULT, or status code           |
| INFO in any hot path or per-item detail       | Floods the log                    | Demote to DEBUG; emit one INFO summary line instead.  |
| printf/fprintf/cerr instead of Log()         | Bypasses structured logging       | Use Log() from logging.h.  Exception: `nevr_boot_log_tee::TeeFprintf` before the game logger exists (Rule 12). |
| Game-native line without an nEVR annotation  | Noise                             | Suppress or wrap with structured fields               |
| Identifiers or outcomes only as message text, or JSON or `key=value` text inside `msg` | A reader must regex them out; no types | Pass each as a typed field (ADR 0027); until the call is migrated, a `key=value` token |
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

Apply these checks to every new `Log()` call, alongside Rules 1-14.

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

// AFTER, structured (ADR 0027) — a JSON true or false
Log(Level::Debug, Tag::Ws, "frame_forwarded", {Bool("success", ok)});
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
// BEFORE — reports an unexplained failure count
[NEVR.PATCH] boot complete: 14 hooks installed, 1 deferred, 1 known-failed, 0 unexpected

// AFTER
[NEVR.PATCH] boot complete: 14 hooks installed, 1 deferred, 1 known-failed
  (optional target unavailable; patch skipped), 0 unexpected
```

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

- The `log_filter.dll` *plugin* is superseded by the built-in
  filter and is refused by the loader. `src/runtime/log/builtin_filter.cpp`
  is the shipping path; do not reintroduce the plugin.
- **`AGENTS.md`** — project conventions, `Log()` usage, subsystem architecture,
  and the C++ Mingw Addendum's structured-output rule.
- **`src/core/logging.h`** — `Log()` and `FatalError()` declarations.
- **`src/core/logging.cpp`** — `Log()` implementation.
- **`docs/adr/0027-structured-logging.md`** — the record schema, the structured `Log()` call, the console
  rendering and the migration of every `Log()` call.

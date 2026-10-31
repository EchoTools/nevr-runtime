# ADR 0027: structured logging: a record names its event, and each field is its own JSON key

Status: proposed. Tracking issue: #841. Nothing in this record is built when it is written. It supersedes the sub-section
"`Log()` does not emit JSON, and that is a decision — not an omission" of Rule 7 in `docs/standards/logging.md`; the pull
request that adds this record replaces that sub-section, and the passages of the standard that present fields as text
inside a message, with the rule below. Related: #570 and #1232 (where the boot log is written), #857 and ADR 0012 (the logs
folder under the nevr root), #579 (the Quest sentinel's fields), #598 (plugins log through the host). The counts are
measured over the tree this record is added to; "What is not known" lists what no measurement covers. Sources are cited by
the names in "Sources", each with the passage relied on.

## Requirement

A call names a level, a subsystem, an event and typed fields, `Log(level, subsystem, event, {fields})`, and the sink writes
each field as its own JSON key, never as text or JSON inside `msg`. A record carries a schema version and the run id (N80).
PC and Quest write the same record (#841). The console shows each record as one readable UTF-8 line. Tests assert fields,
not text.

## Context

### A record is text inside a JSON envelope

`Log(EchoVR::LogLevel, format, ...)` (`src/core/logging.h`) formats printf text. In the boot phase (N36) it goes to the boot
sink, `nevr_boot_log_tee`, which writes `nevr-boot.jsonl`. After the boot phase it calls the game's `EchoVR::WriteLog`; the
built-in filter's detour on the game's `CLog::PrintfImpl` (`hook_PrintfImpl` in `src/runtime/log/builtin_filter.cpp`)
captures the formatted text, and `EmitLine` writes it to the console and to the run log (`WriteFileRecord`). Before the
game's logger exists, `Log()` writes to stderr. A run-log record:

```json
{"ts":"2026-01-02T03:04:05.678Z","run":"<N80 run id>","level":"info","msg":"[NEVR.WS] login injected xpid=OVR-ORG-<id> platform=4 conn=1 (<label>) size=<n>"}
```

The subsystem, the event and every identifier are prose inside `msg`, and a reader regexes them out. Two more facts of
this path matter. Our own record reaches the run log only through the game's `CLog::PrintfImpl` hook, so when that hook is
not installed or another module took it (two of the three causes the filter's zero-game-lines warning names), our lines
miss the file. And `Log()` carries no `format(printf, ...)` attribute, so `-Wformat` checks none of its calls.

Three writers in the tree already write fields as keys, each with a header of its own:

| Writer | Header | Fields |
| --- | --- | --- |
| host, `nevr-bootstrap.jsonl` (`nevr_host::JsonLine` in `src/runtime/host/host_json.h`; `BootstrapDiagnostics::EmitChecked` in `src/runtime/host/bootstrap_diagnostics.cpp`) | `tag` `NEVR.HOST`, `ts`, `pid`, `run` (the host's own 16-hex-digit token), `event` | flat: string, signed and unsigned integer, `0x` hex string, bool, byte string |
| installer run log (`installer/src/log.rs`, ADR 0023) | `tag` `[NEVR.INSTALL]` (brackets included), `ts`, `level`, `run_id`, `event`, `step`, `platform` | flat; a reserved key is refused |
| Quest sentinel (`src/quest/sentinel/hook_log.h`) | `ts_ms`, `level`, `event` | flat: a string or a `long long`; a `bool` is written `0`/`1`, an unsigned value is cast to `long long`, a non-ASCII byte becomes `?` |

### Measured

Counts cover the live sources and count call sites, not comments, declarations or definitions. The rows that
`tools/log_call_census.py <rev>` recounts are reproduced by it at `f41d66b4e0de7e5430bda7a133491be23dad5417` (the
command reads the tree with git only); each recounted row names its scope. The rows marked "not recounted" have no
script yet.

| What | Count |
| --- | --- |
| printf `Log(` calls outside `src/quest/`, `src/runtime/tests/` and `Router::Log` | 1,045 in 63 files: `src/runtime` 983, `src/modules` 54, `src/core` 7, `plugins/common` 1 |
| by level | Info 374, Warning 348, Debug 173, Error 110, an expression (a ternary or a variable) 40 |
| by format (not recounted) | 225 literals with no conversion; 769 with conversions; 48 pass a string built elsewhere through `"%s"`; 3 formats are not literals |
| subsystem (not recounted) | 36 leading tags; 57 calls carry none (the 48 `"%s"` calls and 9 literals). 13 tags in code are not rows of the standard's table: `NEVR.MIC` (61 calls), `NEVR.SOCIAL` (60), `TELEMETRY.DIAG` (32), `NEVR.SERVERDB` (28), `NEVR.ASSET` (13), `NEVR.PARTY` (9), `NEVR.TRACE` (8), `NEVR.ROOT` (8), `NEVR.BOOT` (5), `NEVR.COOPAI` (5), `NEVR.SERVER` (4), `NEVR.EARLYQUIT` (3), `NEVR.LOBBY` (3) |
| identifiers in the text (not recounted) | 523 formats carry a `key=` token; 438 distinct keys (`name` 80, `reason` 66, `va` 51, `error` 33, `conn` 30) |
| text keys that name a header key (not recounted) | `msg` 3, `level` 1, `event` 1, `tag` 1; `error` 33 and `plugin` 9, which section 2 keeps as field keys |
| non-ASCII in formats (not recounted) | 187 formats: U+2014 (em dash) in 186, U+2192 in 1 |
| other writers | `nevr_boot_log_tee::TeeFprintf` 84 calls (54 in `src/runtime/lifecycle/initialize.cpp`); the filter's `BlfLog` 12; `ServerFatal` 11; `FatalError` 10; `VehPrintf` 38 (26 on the runtime's crash path, N70; 12 in the unbuilt `plugins/crash-handler`); `fprintf`/`vfprintf` to stderr 49 outside `src/quest/` (tests, the launcher and the script checker included), 3 of them in `src/runtime/server/gameserver.cpp` printing a `%p` |
| second loggers | a file-local `Log` in `src/runtime/server/messages.cpp` (10 of the 1,045, not recounted) calls `EchoVR::WriteLog` directly, so it skips the boot sink; `Router::Log` in `src/runtime/compat/session_router.cpp` (43 calls) writes to a sink the router is given |
| exception text in a record | 3 calls pass `what()` (`src/runtime/patch/resource_override.cpp`, `src/runtime/patch/invite_wizard_hook.cpp`, `src/runtime/patch/invite_choice_hook.cpp`); Rule 4 names callback exception text |
| plugins | `PluginLog`, `PluginLogWarning`, `PluginLogError`, `PluginLogDebug`, `PluginLogFields`: 76 calls in `src/` and `plugins/` outside tests (the census's scope; the earlier figure, 88, is not reproduced) |
| Quest | `LogFields`/`LogFieldList` 233 calls under `src/quest/` outside tests (the census; the earlier figure, 235, is not reproduced); 74 calls to local `Log` functions (not recounted): members that forward a text line to the sink they are given (`src/quest/auth/session.cpp` 35, `src/quest/net/loopback_game_server.cpp` 19, `src/quest/net/remote_ws.cpp` 6) and the autopilot plugin's helper that calls the host's plugin log (`src/quest/plugins/autopilot/plugin.cpp` 14); 9 `__android_log_*` calls outside tests and stubs |
| tests | 251 uses of text-search helpers (`TestLogContains`, `TestLogContainsAtLevel`, `LogLinesContaining`, `LogContains`, `WaitForLogLines`) in test directories (the census; the earlier figure, 256, and its per-helper split are not reproduced); 5 test files link a `Log` of their own (not recounted) |

The level split is reproduced exactly by:

```sh
rg --pcre2 -U -o --no-filename '(?<![A-Za-z0-9_.>:])Log\(\s*EchoVR::LogLevel::(Debug|Info|Warning|Error)\b' -r '$1' \
  -g '!src/quest/**' -g '!src/runtime/tests/**' -g '!src/runtime/compat/session_router.cpp' \
  src plugins | sort | uniq -c
```

Recount and scope: `python3 tools/log_call_census.py f41d66b4e0de7e5430bda7a133491be23dad5417` prints every recounted row.
Its regexes skip comment lines, trailing `//` comments and definitions (a return type before the name); the rows it does
not cover are marked "not recounted" above.

### Who reads a record

| Reader | What it reads |
| --- | --- |
| `tests/smoke/score-log.sh` | 83 rows of regexes over the log after ANSI is stripped; 21 name a `[NEVR.` tag (`[NEVR.WS] LOGIN SUCCESS`, `[NEVR.WS] login injected xpid=`, `[NEVR.LOGFILTER] health emitted=`), and others match our text without one (`boot log opened run=`) |
| `verify-server.sh` | its console capture, by 8 phrases (`LOGIN SUCCESS`, `registration successful`, `Proxy listening on`, `login injected`, `Graceful shutdown initiated`, `Service redirect`, `HTTP(S) connection redirected`, `engine flags`) |
| `launch-client.sh` | the newest run log, by 6 phrases (`to logged in`, `rad15_live failed`, `Service is unavailable`, `built-in defaults embedded in this build: (none)`, `to in game`, `invalid header`) |
| `AGENTS.md`, "Verification before merge" | the four keys on the `built-in defaults embedded in this build:` line, then `LOGIN SUCCESS` and `to logged in` |
| `tools/scenario/run_scenario.py`, `tools/scenario/scenarios/` | 131 regex steps (78 `expect_log`, 53 `wait_log`) in 21 files over the console capture; the `[NEVR.SCENARIO]` marker |
| `tools/host_stop_cause.py` | host records, by key (`event`, `run`, `outcome`) |
| `tools/quest_autopilot_run.py` (`parse_plugin_line`) and `tools/tests/test_quest_autopilot_run.py` | the autopilot plugin's `[NEVR.PLUGIN] plugin=nevr_autopilot <message> <key>=<value>` lines in logcat; the test's fixtures also carry the sentinel's `ts_ms` records |
| `tools/winvm/checks.py` | the guest run's log, by regexes over our text (`[NEVR.PATCH] FATAL hooking init failed`, `All hooks installed`) |
| `just verify` sensors | the text or the form of calls, among them N36, N47 and N94, N70, N77 to N81, N90, N120, N126 to N129, D1, the Rule 2 sensor (N91: `login injected` at `Log(EchoVR::LogLevel::Info`) and the #515 sensor (printf `Log(` calls and log text in `config.cpp` and `boot.cpp`) |
| `tools/verify_log_rules.py` | `LogLevel::Info` inside `DispatchPerFrameWork` |
| `tools/tests/` | 24 test files and 2 fixtures name `[NEVR.` or `LogLevel::`: the files that pin log text or a call's form are among them, not counted one by one |
| GTests | the substring helpers and `Log` stubs above |

### The superseded sub-section

Its reason is that our lines go through the game's logger, so a second JSON format would double every line. The run log
is JSON already, written by the filter; what it lacks is the fields as keys. This design writes one stream, the filter's
run log, through one encoder; the console line is a rendering of the record, not a second log, and our records stop going
through the game's `CLog` at all. journald fields draws the same line between the sentence and the data: `MESSAGE=` "is not
supposed to be parsed for metadata"; OTel keeps the body and the attributes apart. The standard's sub-section "A record's
fields are JSON keys (ADR 0027)" replaces it.

## Decision

### 1. The call

A header and a source in `src/core/`, namespace `nevr_log`, platform-neutral (no `<windows.h>`), so the PC runtime and the
Quest build compile the same code; `src/quest/CMakeLists.txt` already compiles platform-neutral `src/core/` sources.

```cpp
namespace nevr_log {

enum class Level : std::uint8_t { Debug, Info, Warning, Error };

// One entry per row of the "Subsystem Tags" table of docs/standards/logging.md; TagName(Tag::Ws) is "NEVR.WS".
enum class Tag : std::uint8_t { Ws, GameServer, Patch, Headless, Module, Plugin, Telemetry, Xpid, Config, Crash, Auth /* ... */ };

// A string literal: snake_case ASCII, at most 48 bytes. A std::string or a const char* does not convert; the source
// check (Tests, 4) makes sure the array is a literal, so text known only at run time is never an event or a msg.
class Event {
 public:
  template <std::size_t N> constexpr Event(const char (&name)[N]) noexcept : name_(name) {}
  const char* name() const noexcept { return name_; }
 private:
  const char* name_;
};
class Msg;    // the same shape: a literal sentence, or none

class Field;  // a literal key, a kind and a value; a string is copied into the record before Log() returns
template <std::size_t N> Field Str(const char (&key)[N], std::string_view value) noexcept;     // string, UTF-8 checked
template <std::size_t N> Field Int(const char (&key)[N], std::int64_t value) noexcept;         // number
template <std::size_t N> Field UInt(const char (&key)[N], std::uint64_t value) noexcept;       // number: a size, a count
template <std::size_t N> Field Bool(const char (&key)[N], bool value) noexcept;                // true or false
template <std::size_t N> Field Num(const char (&key)[N], double value) noexcept;               // number; NaN, infinities as strings
template <std::size_t N> Field Hex(const char (&key)[N], std::uint64_t value) noexcept;        // "0x..": an image address, a hash
template <std::size_t N> Field Id(const char (&key)[N], std::uint64_t value) noexcept;         // decimal string: an account, a session
template <std::size_t N> Field HexBytes(const char (&key)[N], const std::uint8_t* bytes, std::size_t count) noexcept;  // "4883ec28"
template <std::size_t N> Field Len(const char (&key)[N], std::string_view text) noexcept;      // <key>_bytes: the size, never the text
template <std::size_t N> Field Fp(const char (&key)[N], std::string_view secret) noexcept;     // <key>_fp: 8 hex digits, salted by run
template <std::size_t N, typename T> Field Bool(const char (&key)[N], const T* pointer) = delete;  // a pointer is not a flag

bool Enabled(Level level) noexcept;  // one relaxed atomic load
}  // namespace nevr_log

void Log(nevr_log::Level level, nevr_log::Tag tag, nevr_log::Event event,
         std::initializer_list<nevr_log::Field> fields = {}, nevr_log::Msg msg = {}) noexcept;
```

Fields are built by named constructors, not by braced `{"key", value}` pairs. A `std::uint64_t` is a count, an image
address or an identifier, and only the constructor's name can say which. With a braced pair the value's C++ type picks the
kind: the Quest sentinel's `LogField` writes a `bool` as `0`/`1` and casts a `uint64_t` to `long long`, and a pair type
that adds a `bool` alternative takes every pointer as a `bool` without a word (a standard conversion). zap ("Structured
context as strongly typed Field values"), slog (a `Kind` per value, `KindUint64` apart from `KindInt64`) and TraceLogging
(`TraceLoggingInt32`, `TraceLoggingString`) type each field at the call. The event name is a literal, as
TraceLoggingWrite requires ("This must be a string literal and not a variable"). No constructor takes a pointer
value: `Hex` takes an integer, so a heap address needs an explicit `reinterpret_cast` the source check sees, and the deleted
`Bool` overload closes the one implicit pointer conversion. A Debug call with an expensive argument is guarded with
`if (nevr_log::Enabled(Level::Debug))`: a function's arguments are evaluated before the call, and the API has no macro that
could skip them (the addendum: "Use the language, not macros"); TraceLoggingWrite evaluates them "if and only if the event
is enabled" only because it is a macro.

A literal-only event, and what it writes:

```cpp
using namespace nevr_log;  // in a .cpp, never in a header

Log(Level::Info, Tag::Ws, "login_success");
```

```json
{"schema":1,"ts":"2026-01-02T03:04:05.678Z","run":"<run id>","level":"info","tag":"NEVR.WS","event":"login_success"}
```

Events with typed fields: a string, an unsigned code, two bools and a hex value that is not a pointer at Info; a signed
connection index and an unsigned size; a double. Each comment is the printf format the call replaces, and the arguments
are the names at those call sites in `src/runtime/compat/ws_bridge.cpp` and `src/runtime/lifecycle/config.cpp`.

```cpp
// "[NEVR.WS] login platform=%d (%s) no_vr=%d game_flags=0x%x is_server=%d"
Log(Level::Info, Tag::Ws, "login_platform",
    {UInt("platform", platformCode), Str("prefix", nevr_evr_codec::PlatformPrefix(platformCode)),
     Bool("no_vr", GameIsNoVr()), Hex("game_flags", flags ? *flags : 0u), Bool("is_server", g_isServer != FALSE)});

// "[NEVR.WS] login injected xpid=%s platform=%d conn=%d (%s) size=%zu"
Log(Level::Info, Tag::Ws, "login_injected",
    {Str("xpid", xpid), UInt("platform", injectedPlatform), Int("conn", connIdx), Str("conn_role", ConnLabel(connIdx)),
     UInt("bytes", injectedSize)});

// "[NEVR.PATCH] arena_round_time override=%.0fs source=config.yaml"
Log(Level::Info, Tag::Patch, "config_override",
    {Str("key", "arena_round_time"), Num("value_s", g_arenaRoundTime), Str("source", "config.yaml")});
```

```json
{"schema":1,"ts":"2026-01-02T03:04:05.678Z","run":"<run id>","level":"info","tag":"NEVR.WS","event":"login_platform","platform":4,"prefix":"OVR-ORG","no_vr":false,"game_flags":"0x0","is_server":false}
```

The printf form, `Log(EchoVR::LogLevel, format, ...)`, keeps working until the end state (section 9) and gains
`__attribute__((format(printf, 2, 3)))`.

### 2. The record

One JSON object per line, UTF-8, flat. A field is never an object, an array, or JSON or `key=value` text inside `msg`.

| Key | JSON type | Meaning | Set by |
| --- | --- | --- | --- |
| `schema` | number | the record schema version, `1`; a change of a header key's meaning or type raises it | the encoder |
| `ts` | string | UTC time of the call, ISO 8601 with milliseconds and `Z`; always set, whatever the console shows | the encoder |
| `run` | string | the run id, `GetRunId()` (N80) | the encoder |
| `level` | string | `debug`, `info`, `warn` or `error` (the spellings the run log uses now) | the call |
| `tag` | string | the subsystem, a row of the standard's table, without brackets (`NEVR.WS`); `EVR` on a game line | the call; the sink, from the text's leading tag, for a printf call |
| `event` | string | the class of event, snake_case; `text` for a printf call; `game_line` for a game line | the call |
| `msg` | string, optional | a literal sentence that states the consequence (Category G); the formatted text of a printf call; the text of a game line | the call |
| `src` | string, optional | `boot` on a record replayed into the run log from the boot spool | the filter |
| `plugin` | string, optional | the plugin's id, the `name` of its `config.yaml` entry: on every plugin record, and on a loader record about one plugin | the host; a loader call |
| `fields_dropped`, `fields_truncated` | number, optional | fields refused (a reserved key, over the record cap) and string values cut | the encoder |
| `record_error` | string, optional | `record_too_large` when the record did not fit: the header, the event and this key only | the encoder |
| every other key | string, number or bool | a field of the event, snake_case ASCII | the call |

- **One event, one shape.** A (`tag`, `event`) pair always carries the same keys with the same kinds. OTel's EventName
  "SHOULD uniquely identify the event structure (both attributes and body)"; messagetemplates treats "the template as an
  event type". A source check in `tools/tests/` reads every call and fails on a key with two kinds under one pair, which
  makes Category H (a field's name matches what it holds) a check.
- **Reserved keys.** The header keys above cannot be field keys. A literal one at a call site fails the source check; one
  that arrives at run time (a plugin's key) is dropped and counted in `fields_dropped`. Precedent: the installer refuses a
  reserved key; journald fields are "trusted fields ... that cannot be altered by client code" when they start with `_`.
  The header's failure key is `record_error`, not `error`, because 33 formats use `error=` as a field; `plugin=` (9
  formats, the plugin loader's lines) already means the plugin's id, so it is one key with one meaning, and only a plugin
  cannot set it.
- **Numbers are quantities; identifiers are strings.** `Int`, `UInt` and `Num` write JSON numbers. RFC 8259 holds integers
  interoperable only in [-(2**53)+1, (2**53)-1], so a 64-bit identifier (an account, a session, a symbol) is `Id` (a decimal
  string) or `Hex` (a `0x` string), as protobuf's JSON mapping writes every 64-bit integer as "a decimal string". NaN and
  the infinities are not JSON numbers (RFC 8259) and are written `"NaN"`, `"Infinity"`, `"-Infinity"` (protobuf JSON).
- **UTF-8.** Every string is checked, and an invalid sequence becomes U+FFFD (RFC 8259: JSON exchanged between systems
  "MUST be encoded using UTF-8"; the host's `Wide` and its `nlohmann::json` `error_handler_t::replace` do the same). The
  run log's escaper (`src/core/json_escape.h`) passes every byte at or above `0x20` unchecked. Event names, keys and `msg`
  literals are ASCII (a source check); values may be any UTF-8.
- **Size.** A record is encoded into an 8 KiB buffer on the caller's stack, the size the filter uses for one game line;
  nothing is allocated. A string value longer than 1 KiB is cut at a character boundary and counted in `fields_truncated`.
  A record that still does not fit is never written cut: it becomes its header, `event` and `record_error` (as
  `JsonLine` writes `error`).
- **Encoder.** One hand-written encoder, generalised from `nevr_host::JsonLine` (fixed buffer, no heap, no CRT
  formatting; `Num` uses `std::to_chars`). This is a deviation from AGENTS.md "No hand-built serialization", for the same
  reason `host_json.h` and `hook_log.h` state: the boot and fatal paths cannot allocate. It is paid for as they pay: every
  test parses every record it captures with `nlohmann::json`.

### 3. Subsystem and event: one vocabulary

The `Tag` enum is the standard's "Subsystem Tags" table, row for row; a test in `tools/tests/` parses the table and the
enum and fails on any difference, so a tag exists in both or in neither. The pull request that adds the enum adds the 13
tags the code uses and the table lacks (Open question 3). The record carries the tag without brackets, as the host writes
`NEVR.HOST`; the console shows it in brackets, as every line does now. A printf call keeps its bracketed tag in its text,
and the sink reads the tag from there when it is a row of the table.

An event name says what happened, in snake_case (`login_injected`, `hook_failed`, `health`). It is unique within its tag,
and its text is never built at run time.

### 4. Redaction (Rule 4): the safe form is the short form

- Remote text (a response body, a close or failure reason, a parser's or a callback's exception text) is passed to `Len`,
  which writes only its size as `<key>_bytes`: `Len("body", response.body)` is shorter than any way to write the body.
- A value that must stay private but must be matched across records (an invite code) is passed to `Fp`, which writes 8
  hex digits of a hash salted with the run id as `<key>_fp`; `CodeFingerprint` in `src/runtime/patch/invite_wizard.h` is
  the precedent (FNV-1a over a salt that differs from run to run).
- A URL is `Str(key, RedactUrlForDiagnostics(url))` (`src/runtime/log/url_diagnostics.h`). A secret config value is never a
  field; its presence is (`Bool("nevr_http_key_set", ...)`).
- The source check fails a call whose arguments contain `what()` or a JSON `dump(`, or a `Str` whose key is `token`,
  `password`, `secret`, `authorization`, `cookie`, `body`, `response`, `payload` or `what`, or a `Str` whose key names a URL
  (`uri`, `url`, `*_uri`, `*_url`) and whose value does not pass through the redacting formatter. slog's `LogValuer` is the
  precedent for making the safe value the one a type produces ("the Value returned from its LogValue method is used").

### 5. Where a record goes

`Log()` checks the level (one relaxed atomic load), encodes the record, and hands the bytes to one sink, by phase:

1. **Boot phase** (`nevr_boot_log_tee::Init()` to `Close()`, N36): the boot spool, `nevr-boot.jsonl`, and the console, both
   with kernel32 `WriteFile`. That makes N43's "kernel32 calls only (no CRT locks)" true of the console mirror too:
   `TeeFprintf` mirrors to stderr with the CRT's `vfprintf` and `fflush`, which take the stream lock.
2. **After the boot phase**: the run log's writer in the built-in filter, which the filter sets at its init (as the boot
   tee sets the boot sink), and the console rendering. Our records no longer pass through `EchoVR::WriteLog`; the game's
   `CLog::PrintfImpl` hook carries game lines only. With `passthrough_to_engine` set, the console rendering is also handed
   to the game's original `PrintfImpl`, with `%` escaped as `hook_PrintfImpl` does for a cut line: that is the game's logger
   as a PC sink (#841).
3. **Before either exists**: the console rendering only.

One record is one `WriteFile` under one lock. The addendum asks for a lock-free hot path with per-thread buffers; this is a
stated deviation (Open question 4): a per-thread buffer needs a flushing thread and loses what it holds at
`TerminateProcess`, which is how a server ends (`ForceFatalExit`), and Rule 13 keeps Info off the per-frame path. A write
that fails is counted, the health record reports the count as `write_failed`, and the first failure in a run is also put on
the console (AGENTS.md, Logging, rule 4). The crash path (`BreakpointVEH`, `WriteCrashDump`) does not call `Log()` and keeps
`VehPrintf` (N70). `ServerFatal` and the fatal handler write one Error record (`tag` `NEVR.FATAL`, `event` `fatal`, the
cause as fields) before `ForceFatalExit`.

The replay of the boot spool into the run log (`ReplayBootLines`) copies a record and adds `src`; a line in the spool's
older shape (the spool keeps every run) is wrapped as now.

### 6. The console

```
<ts> <level> [<tag>] <event> <key>=<value> ... msg="<msg>"
```

A record of the printf form renders as now, `<ts> <level> <msg>`, the tag inside `msg`; a game line also renders as now.
A value is written bare unless it is empty or holds a space, `=`, `"` or `\`, when it is quoted with `"` and `\` escaped
(the rule of `src/runtime/ext/plugin_log.h`; slog's TextHandler writes `key=value` "easily and unambiguously parsed by
machine"). A control character is written as an escape, so one record is one line. Booleans are `true`/`false`, numbers in
decimal, `Hex` as `0x..`. `timestamps` and `console_color` keep their meaning; the file never carries ANSI.

The em dash is in 186 formats. Its UTF-8 bytes `E2 80 94` read in code page 437 are `ΓÇö`, the text a Windows console shows
for it. The fix point is where the runtime makes its console, `PatchEnableHeadless` in `src/runtime/patch/mode_patches.cpp`
(`AllocConsole`, then `freopen_s` of `CONOUT$`): `SetConsoleOutputCP(CP_UTF8)` there. That console exists only in headless
mode (`boot.cpp` calls `PatchEnableHeadless` when `g_isHeadless` is set) and not with `-no-console`; no
`SetConsoleOutputCP` call exists in `src/`. Under Wine the runtime makes no console (`-no-console` is set when `ntdll`
exports `wine_get_version`, in `src/runtime/lifecycle/boot.cpp`), so the bytes reach the Unix stream as written. These
statements come from reading the code; neither the mojibake nor the fix has been seen in a run.

### 7. The filter and collapse

- **Game lines** become records with `tag` `EVR`, `event` `game_line`, the level the game gave, and the game's text as
  `msg`, annotated as now.
- **N77.** Our records never enter `hook_PrintfImpl`, so `suppress_patterns`, which match game text, cannot delete them;
  the exemption holds by construction, and `IsNevrLine` stays as the defence for a `[NEVR.` line that still arrives
  through the game's logger. `min_level` applies to every record; a `suppress_channels` entry matches a game line's text
  prefix, or a record whose `tag` is the entry without brackets.
- **Truncation.** `max_line_length` and `truncate_patterns` cut the `msg` of a game line and of a printf record, at a
  character boundary, and the record says so with `msg_truncated_bytes`. A structured record is bounded by section 2.
- **N78.** A structured record's identity for collapse is its level, tag, event, and the keys and values of its `Str`,
  `Bool`, `Hex`, `Id` and `HexBytes` fields; `Int`, `UInt` and `Num` fields count by key only. Quantities fold and
  identities do not, the structured form of the digit folding `src/runtime/log/rate_limiter.h` applies to text, which stays
  for printf records and game lines. zap samples by "the same level and message"; journald's rate limit reports "the number
  of dropped messages". The summary becomes a record: `tag` `NEVR.LOGFILTER`, `event` `repeated`, `count`, `window_s`,
  `repeated_level`, `repeated_tag`, `repeated_event`, and for text `repeated_msg`, the first line.
- **N79, N88.** The health line becomes `event` `health` with `emitted`, `suppressed`, `suppression_pct`, `game_lines`,
  `interval_s` and `write_failed`; the zero-game-lines warning becomes `event` `zero_game_lines` with `cause`
  (`hook_not_installed`, `hook_taken`, `game_idle`). Both stay driven from the per-frame tick.

### 8. Plugins

The ABI does not change (`NevrLogFn`, `NevrLogField` in `sdk/include/nevr/plugin_interface.h`; `NEVR_PLUGIN_API_VERSION` 8).
`HostPluginLog` (`src/runtime/ext/plugin_loader.cpp`) writes a record with `tag` `NEVR.PLUGIN`, `plugin` (set by the host,
so one plugin cannot write as another, Rule 15), `event` `plugin_line`, the plugin's message as `msg`, and each
`NevrLogField` as its own key with a string value: the ABI carries strings, and reading `"5"` as a number would let one key
change kind between records. A key is cleaned as `src/runtime/ext/plugin_log.h` cleans it now (a character outside the
identifier set becomes `_`); a key that is then reserved is dropped and counted; the caps of `plugin_log.h` hold. The
console rendering is the line Rule 15 shows now.

### 9. Migration, in stages

Each stage is pull requests with their own issues; a call site and every reader of its text move in the same pull request.

1. **The core.** The encoder, the fields, the `Tag` enum and its table test, the PC sink of section 5, and the console
   code page. The printf `Log()` is routed into it: its formatted text is `msg` byte for byte, `tag` is read from the text,
   `event` is `text`, `msg` stays the last key. Every reader of section "Who reads a record" keeps working, because the
   text it matches is unchanged and still on one line. The shadow `Log` in `src/runtime/server/messages.cpp` goes (its 10
   calls use the global one, so they reach the boot sink). The structured sink is the only JSON writer. The printf form gains the format attribute.
   A ceiling check in `tools/tests/` holds the printf call count and fails a pull request that raises it, so new code uses
   the structured form; each migration lowers it.
2. **Boot, bridge and login, config, fatal**, in that order. Boot: `src/runtime/lifecycle/initialize.cpp` and the other
   `TeeFprintf` sites, `src/runtime/lifecycle/boot.cpp`, `src/runtime/log/boot_log_tee.cpp`. Bridge and login: the login,
   proxy and service lines of `src/runtime/compat/ws_bridge.cpp` and `src/modules/token-auth/`. Config:
   `src/runtime/lifecycle/service_config.cpp`, `src/runtime/lifecycle/config.cpp`. Fatal: `ServerFatal`, the fatal handler
   and the shutdown lines of `src/runtime/lifecycle/crash_recovery.cpp`. With them move score-log rows, `verify-server.sh`,
   `launch-client.sh`, `tools/winvm/checks.py`, the N36, N94 and N120 sensors, the Rule 2 sensor (N91), the #515 sensor
   and the verification text of `AGENTS.md`.
3. **The rest of `src/runtime` and `src/modules`**, one directory per pull request, with the filter's own lines (`BlfLog`,
   section 7), the plugin records (section 8) and the scenario steps that match the lines that move. `LogFrameMessages` in
   `src/runtime/compat/ws_bridge.cpp` moves to Debug on a server as on a client, and its comment's claim that a server's
   message traffic is sparse goes with it: the claim is stated without a measurement, and the rule against Info on a
   per-connection path governs (the standard's "You SHALL NOT"; Rule 16 is amended to say so). Rule 16's lobby records then run on a server
   too: the server gate in `ObserveSocialFrames`, the comment above it that repeats the claim, and the test
   `AServerGetsNoLobbyRecordFromTheSocialObserver` change in that pull request.
4. **The end state.** No printf `Log()` and no `TeeFprintf` remain, and both are deleted; a game line's `msg` drops the
   `[EVR] ` prefix, which `tag` carries. The ceiling check becomes the sensor that forbids the old form: no
   `Log(EchoVR::LogLevel` call, no `TeeFprintf`, and no `fprintf` or `vfprintf` to stderr in
   `src/runtime`, `src/core` and `src/modules` outside tests, the console renderer and the crash path. It runs in
   `tools/tests/`, so every pull request runs it, and a `just verify` sensor names it under the next free N-number, built as N93, N99 and
   N111 require (two stages, comments stripped). D1 is re-pointed at the one definition of the structured `Log`.

In the mixed state a run log holds records of both forms, told apart by `event`: `text` is a printf call, `game_line` a
game line, anything else is structured. A reader that matches text keeps working on unmigrated lines; a reader of a migrated
event reads its keys.

### 10. Each reader

| Reader | What happens |
| --- | --- |
| `tests/smoke/score-log.sh` | unchanged by stage 1; a row moves to a key match (`"event":"login_success"`) in the pull request that migrates its line; the row's comment keeps the regex's "verified against real output" rule |
| `verify-server.sh`, `launch-client.sh`, `tools/winvm/checks.py` | the same: a phrase becomes the migrated line's console form (`login_injected xpid=...`) or a key match over the run log when its line migrates; `to logged in` and `rad15_live failed` are game lines and stay text |
| `AGENTS.md` verification text | stage 2 rewrites it to the config and login events, with the scripts |
| scenario steps | a `wait_log` or `expect_log` step keeps its regex over the console until its line migrates; the console form of section 6 keeps a migrated line matchable by `event` and `key=value`, and the harness gains a key match over the run log |
| `tools/host_stop_cause.py` | not affected: the host's records keep their writer |
| `tools/quest_autopilot_run.py` and its test | move with the Quest stage and with the plugin records (section 8) |
| `just verify` sensors, `tools/verify_log_rules.py`, the `tools/tests/` files | each is re-pointed in the pull request that changes the text or form it reads (Rule 14) |
| GTests | a capturing sink replaces the substring helpers and the `Log` stubs as their files migrate |

### 11. Quest

The Quest build compiles the same encoder and writes the same record; only the sinks are its own, the sentinel's on-disk
log and logcat (NDK logging: `__android_log_write` "Writes the constant string text to the main log buffer, with priority
prio ... and tag tag"). `LogFields(level, event, {fields})` maps one to one onto `Log()`: a string becomes `Str`, an integer
`Int` or `UInt` by its type, and a `bool` `Bool`, which ends the `0`/`1` and the cast of unsigned values. `ts_ms` becomes
`ts`; its readers are two Quest tests (`src/quest/tests/got_hook_test.cpp`, `src/quest/tests/sentinel_host_test.cpp`),
the sentinel's on-disk log (`src/quest/sentinel/sentinel_log.cpp`, which drops the key) and the fixtures of
`tools/tests/test_quest_autopilot_run.py`. The 74 calls to local `Log` functions and `Router::Log` (compiled into the
Quest build too) move to the structured call through the sinks they are given. The Quest has no N80 run id; the
record's `run` there is the process id with the process start time from `/proc/self/stat`, formatted as on PC (not built
or measured). The Quest stage starts once stage 1's core exists and
retires the sentinel's own writer, as #841 requires; it is the platform twin of stages 2 to 4, not a later option.

### 12. Seams this design does not own

- **Where logs are written.** The install-root work owns it (ADR 0012, #857, #570, #1232). The sink takes the folder from
  `nevr_root::LogsDir`, as the filter does now, and changes no path.
- **Retention (decided).** The run logs follow journald's model: every run's log is kept, and the oldest are deleted only
  when the logs folder passes a size cap, 1 GB by default on a client and no cap by default on a server; the open file is
  never deleted. journald.conf: `SystemMaxUse=` "defaults to 10% ... of the size of the respective file system, but each
  value is capped to 4G", "only archived files are deleted to reduce the space occupied by journal files", and
  `MaxRetentionSec=` is off by default. This replaces the count of `rotate_keep` (5) in `PruneOldLogs`; the change is its
  own pull request.
- **Per-request latency records** (AGENTS.md, Logging, rule 1). Out of scope; they are records of this schema written at
  the request choke point, with fields such as `latency_ms` and `bytes`.
- **Flight recorder** (the addendum's ring buffer). Out of scope; the seam is the sink, which holds each complete encoded
  record and can keep the last N for the crash path to write raw, without `Log()` (N70).

## Tests, failing first

1. **Encoder** (GTest under Wine for PC; the Quest host tests): every field kind, escaping, invalid UTF-8, a reserved key,
   the string cap, `record_too_large`, NaN and the infinities, the largest and smallest 64-bit values; each output parsed
   with `nlohmann::json`, and its keys and JSON types asserted.
2. **Capturing sink.** A test installs a sink that keeps parsed records and asserts by key and type
   (`HasRecord(Tag::Ws, "login_injected", {Str("xpid", "OVR-ORG-1")})`), never by substring. It replaces
   `TestLogContains` and the link-time `Log` stubs as files migrate. One test drives the same call through the PC and the
   Quest builds and compares the records (#841).
3. **The real binary** (AGENTS.md, Logging, rule 6). A GTest executable under Wine links the real sink, makes one call and
   reads the run-log file it wrote. The stage's live run on a game copy (the PC login test, which runs on main after the release cut) then
   shows, by key, a record with `event` `text`, `tag` `NEVR.WS` and the `LOGIN SUCCESS` text in stage 1, and the
   `login_injected` and `login_success` events from stage 2.
4. **Source checks** in `tools/tests/`: the tag table equals the enum; events and keys are literal snake_case ASCII; no
   reserved key; one shape per (`tag`, `event`); the Rule 4 patterns of section 4; the printf ceiling, then the end-state
   sensor.
5. **Mutants**, each of which a test above must catch: a field dropped from a record; `Hex` written as a number; `Id`
   written as a number; the UTF-8 check skipped; a reserved key let through; a string cut inside a character; a record cut
   instead of replaced by `record_too_large`; our record sent through `EchoVR::WriteLog` (caught by a test that runs with
   the `CLog` hook absent and still finds the record); `msg` of a printf record changed by one byte; the N78 identity
   including an `Int` field (a repeat with a new count stops collapsing); a printf call added (the ceiling).

## Alternatives considered

- **Keep printf, and parse `key=value` out of `msg` in the sink.** slog's TextHandler shows the format can be "easily and
  unambiguously parsed by machine", but only when one writer quotes every value. Our formats are prose with 438 key names,
  unquoted values that hold spaces, labels in parentheses and em dashes; a parse is a guess, and it cannot recover a type.
- **Message templates** (Serilog, CLEF `@mt`, messagetemplates: "the template itself is captured, along with property
  values"). The idea is kept: the event names the template. The syntax is not: C++17 cannot check property names in a
  format string at compile time, and the record would still lead with prose.
- **spdlog.** "Feature-rich formatting, using the excellent fmt library"; its only structured data is MDC, "key-value
  pairs (string values) in thread local storage". A new dependency that stays text.
- **ETW TraceLogging.** Typed fields and literal event names, and "events ... decoded without a manifest" because each
  event carries its names, as each record here does. Windows only, so not the Quest's, and the run log must be a file
  every tool here reads. Its call shape is kept.
- **`nlohmann::json` per record**, as AGENTS.md "No hand-built serialization" asks. A heap allocation and an exception path
  per record, on the boot and fatal paths that cannot have either. Kept for tests, which parse every record.
- **A nested `fields` object** (OTel attributes as a map). The requirement is each field its own key; the host and the
  installer already write flat records; CLEF holds "Any JSON property on the payload object is assumed to be a regular
  property of the event". Reserved keys handle the collisions.
- **Braced `{"key", value}` fields** (the Quest sentinel's `LogField`). Shorter, and wrong in the ways section 1 lists.
- **tracing** (Rust): "Structured fields on spans and events are specified using the syntax field_name = field_value", its
  target defaulting to "the module path where the macro was invoked". A macro language C++ does not have; the module path
  is our `tag`.

## Open questions for the owner

1. **A plugin's event.** Recommended: `plugin_line` with the message as `msg`, as section 8 says, and later a tail member
   of `NvrGameContext` (a capability bump to v9, as v5, v6 and v8 were) that takes an event name and typed fields. The
   alternative, reading a message that looks like an identifier (the Quest autopilot sends `route_start`) as the event,
   changes what a plugin's text means without its author's knowledge.
2. **One `run` per process.** The host stamps its own 16-hex-digit token (pid, creation time and the performance counter,
   mixed, because Wine reuses pids); the runtime stamps N80's pid and creation time. Two values under one key break
   Category H across the host's and the runtime's files. Recommended: the runtime takes the host's token through the
   rendezvous and keeps N80's derivation only when there is no host. Whether the rendezvous can carry it is not checked.
3. **The 13 tags the table lacks.** Recommended: a row for each in the pull request that adds the enum, except
   `TELEMETRY.DIAG`, which breaks the `[NEVR.<AREA>]` rule of `docs/standards/naming.md` and becomes `NEVR.TELEMETRY` with
   its own events when its 32 calls migrate.
4. **The lock.** Recommended: accept the deviation from the addendum's lock-free hot path stated in section 5, and measure
   the cost per record under Wine in stage 1, at idle and under a load that logs.
5. **ASCII `msg` literals.** Recommended: yes, as section 2 says, so the console's code page matters only for values; the
   186 em dashes are rewritten as their lines migrate.

## What is not known

- Where the `ΓÇö` that motivates section 6 was seen, and whether `SetConsoleOutputCP` fixes it there: code reading only.
- Whether `WriteFile` appends from several threads land whole under Wine; the lock serialises our writers in one process.
- The cost of a record (encode and write), idle or under load.
- How many of the 1,045 printf calls break when the format attribute turns on; nothing was built.
- Whether a server's bridge carries lobby find and join frames, so whether Rule 16's records on a server will be written.
- On the Quest: the run id's derivation, `std::to_chars` for `double` in the NDK's library, and logcat's limit on line length.

## Consequences

- A reader finds a record by `tag`, `event` and its keys; tests assert keys and types; Category H can be checked.
- Our records reach the run log without the game's `CLog` hook, so a missing or taken hook costs game lines only.
- Every record repeats its key names, as TraceLogging events "include the event strings (provider name, event name, and
  field names) inside each event"; the retention cap bounds the folder, not the record.
- The migration touches 1,045 printf calls, 84 `TeeFprintf` calls, 12 `BlfLog` calls, the plugin host path and, on the
  Quest, 233 + 74 calls, with every reader of section "Who reads a record"; stage 1 changes no text any reader matches.
- Two deviations are stated, not hidden: the hand-written encoder (section 2) and the lock (section 5).
- Stage 1 edits `src/runtime/log/builtin_filter.cpp`, whose `ParseJsonConfig` holds a `catch (...)`, a Hard Stop of the
  addendum; that pull request names the exception types.
- ADR 0024's runtime lines (`[NEVR.UPDATE] <event> key=value ...`, events such as `update check`) are written with the
  structured call once stage 1 lands, as snake_case events (`update_check`) under a `NEVR.UPDATE` row. ADR 0023's
  installer log keeps its own writer; its bracketed `tag` and its `run_id` differ from this schema's `tag` and `run`.

## Sources

| Name | URL | The passage relied on |
| --- | --- | --- |
| OTel | https://opentelemetry.io/docs/specs/otel/logs/data-model/ | Body: "a human-readable string message ... or it can be a structured data"; Attributes: "Additional information about the specific event occurrence"; EventName: "SHOULD uniquely identify the event structure (both attributes and body)"; SeverityText: "the original string representation of the severity as it is known at the source" |
| slog | https://pkg.go.dev/log/slog | "TextHandler output is a sequence of key=value pairs, easily and unambiguously parsed by machine"; JSONHandler "output is line-delimited JSON"; the `Kind` constants (`KindInt64`, `KindUint64`, `KindFloat64`, `KindBool`, `KindString`, ...); `LogValuer`: "the Value returned from its LogValue method is used for logging" |
| zap | https://pkg.go.dev/go.uber.org/zap | "Structured context as strongly typed Field values"; the Logger "allocates far less, but it only supports structured logging"; sampling "after the first 100 log entries with the same level and message in the same second" |
| CLEF | https://github.com/serilog/serilog-formatting-compact | reified properties `@t`, `@m`, `@mt`, `@l`, `@x`, `@i`; "Any JSON property on the payload object is assumed to be a regular property of the event"; `@@` escaping of a user property that starts with `@` |
| messagetemplates | https://messagetemplates.org/ | "it is possible to treat the template as an event type"; "the template itself is captured, along with property values" |
| journald fields | https://www.freedesktop.org/software/systemd/man/latest/systemd.journal-fields.html | `MESSAGE=`: "is not supposed to be parsed for metadata"; `MESSAGE_ID=`: "for recognizing certain message types"; "Fields prefixed with an underscore are trusted fields ... that cannot be altered by client code" |
| journald.conf | https://www.freedesktop.org/software/systemd/man/latest/journald.conf.html | `SystemMaxUse=` defaults; "only archived files are deleted"; `RateLimitIntervalSec=`: "A message about the number of dropped messages is generated"; `MaxRetentionSec=` default 0 |
| TraceLogging | https://learn.microsoft.com/en-us/windows/win32/tracelogging/trace-logging-about | "decoded without a manifest"; "Structured data: named fields with types"; events "include the event strings (provider name, event name, and field names) inside each event" |
| TraceLoggingWrite | https://learn.microsoft.com/en-us/windows/win32/api/traceloggingprovider/nf-traceloggingprovider-traceloggingwrite | eventName: "This must be a string literal and not a variable"; arguments "will be evaluated if and only if the event is enabled" |
| RFC 8259 | https://www.rfc-editor.org/rfc/rfc8259 | section 6: integers in [-(2**53)+1, (2**53)-1] are interoperable; "Infinity and NaN ... are not permitted"; section 8.1: "MUST be encoded using UTF-8" |
| protobuf JSON | https://protobuf.dev/programming-guides/json/ | int64, fixed64, uint64: "JSON value will be a decimal string"; float and double: "NaN", "Infinity", "-Infinity" |
| SetConsoleOutputCP | https://learn.microsoft.com/en-us/windows/console/setconsoleoutputcp | "A console uses its output code page to translate the character values written by the various output functions into the images displayed in the console window" |
| spdlog | https://github.com/gabime/spdlog | "Feature-rich formatting, using the excellent fmt library"; MDC: "key-value pairs (string values) in thread local storage" |
| tracing | https://docs.rs/tracing/latest/tracing/ | "Structured fields on spans and events are specified using the syntax field_name = field_value"; target defaults to "the module path where the macro was invoked" |
| NDK logging | https://developer.android.com/ndk/reference/group/logging | `__android_log_write`: "Writes the constant string text to the main log buffer, with priority prio ... and tag tag" |

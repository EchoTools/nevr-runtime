# Crash report ingest — wire spec

2026-09-15, Claude, grounded in three haiku-subagent research passes (breakpad/
crashpad upload conventions; nevr-server-rs-work infra inventory; nevr-runtime
crash-payload inventory) plus direct reading of `crash_recovery.cpp`. Written
for Teth to implement the sink on production infrastructure — this defines the
**wire contract** only, not the server-side implementation, storage, or
retention policy, which are hers to decide.

## Why a new sink, not a reused one

Confirmed (subagent, read-only pass over `~/src/nevr-server-rs-work`): that
backend exposes exactly three things — `/metrics` (Prometheus, GET-only, TCP
9090), a UDP 6792 EchoVR client-protocol port, and a WebSocket fleet-manager
control plane (protobuf `Envelope`, `nevr-proto/proto/realtime_v1.proto`).
**No crash, error, telemetry, or diagnostic ingest endpoint exists there.**

The existing telemetry precedent (`TelemetryStreamer`, `src/runtime/server/
telemetry_streamer.h:40-52`) authenticates to a service called "nevr-stream"
(config keys `telemetry.uri`/`telemetry.token`, `src/runtime/lifecycle/
service_map.cpp:62-65`) — but that service's implementation is **not** in
nevr-server-rs-work either; the subagent could not locate it in this repo at
all. So there is no known existing endpoint to extend. This is a new service.
(Open question for Teth in §6: if "nevr-stream" turns out to live somewhere
she already operates, adding a crash route there instead of standing up a new
service may be cheaper — I don't have visibility into that.)

## Architecture constraint: capture and delivery must be separate processes/contexts

This is the one non-negotiable design rule, and it comes directly from code
already in this repo, not from invention. `WriteCrashDump`'s own header
comment (`src/runtime/lifecycle/crash_recovery.cpp:380-384`):

> N70: crash-safe by construction. Every call below is a raw syscall
> (WriteFile / GetStdHandle / VirtualQuery) or a stack-only operation. No
> Log(), no heap, no mutex, no loader-lock call.

A network POST — DNS, TLS, heap allocation, a socket, possibly the loader
lock for a DLL that isn't yet resolved — cannot happen inside this handler
without reintroducing exactly the class of hazard N70 exists to avoid. The
same applies on Quest: breakpad's `DumpCallback` (`src/quest/sentinel/
sentinel.cpp:93-104`) currently does raw fd I/O only (minidump + `/proc/self/
maps`), no network call, and should stay that way.

**Therefore: capture writes to local disk only (already true on both
platforms today), and a separate, ordinary-context uploader reads a spool
directory and POSTs.** This mirrors the real Crashpad architecture (a
persistent out-of-process `crashpad_handler` that never shares fault context
with the crashing process) and is how Firefox/Chrome do it — upload happens
on next launch or from a background thread at a safe point, never from signal/
exception context. Concretely:

- **Windows, client mode:** process typically restarts after a crash. Scan the
  spool directory at next boot, upload anything unsent, then continue normal
  startup.
- **Windows, server mode:** `EnsureStackReserve`/VEH longjmp recovery
  (`crash_recovery.h:9-20`) keeps the process alive after a null-deref AV — the
  upload can run from a normal background thread shortly after recovery,
  still outside the handler.
- **Quest:** the breakpad callback already only writes files. Upload happens
  from the next `libr15` launch (our shim's constructor is the natural place
  to scan for unsent dumps, since it already runs before game code — same
  hook point documented in `docs/design/2026-07-13-quest-crash-reporter-
  injection.md` §1.2).

## Wire format

**HTTP(S) POST, `multipart/form-data`.** This isn't a new invention — it's
the field-for-field convention breakpad/crashpad tooling already expects
(confirmed via Chromium's Crashpad overview doc and a working minimal
reference implementation, `acrisci/simple-breakpad-server`, which accepts
this exact shape with no auth). Reusing the standard field names buys
interop with any existing minidump-aware tooling Teth might want to point at
this later (e.g. `minidump-stackwalk`).

Suggested path: `POST /v1/crash-reports` — Teth's call if her infra has an
existing naming convention to follow instead.

### Fields

| field | type | present when | source |
|---|---|---|---|
| `upload_file_minidump` | binary, `application/octet-stream` | Quest only, today | real breakpad `.dmp`, `sentinel.cpp:114-120` |
| `crash_summary_text` | text | always | the existing `VehPrintf` block verbatim (`crash_recovery.cpp:396-475`) on Windows; the equivalent log lines on Quest |
| `platform` | text | always | `"windows"` \| `"quest"` |
| `prod` | text | always | `"echovr"` (breakpad-convention field name, kept for tool interop) |
| `ver` | text | always | game/build version |
| `client_report_id` | text (UUID) | always | generated client-side; sink's dedupe key |
| `session_id` | text | when available | correlates against existing JSONL/telemetry logs |
| `captured_at` | text (ISO-8601 or unix ts) | always | capture time, not upload time |
| `exception_code` | text (hex) | Windows | `FormatCrashExceptionSummary` param, `crash_dump_format.h:11-13` |
| `rip` / `location` | text (hex) | Windows | absolute RIP and RVA-if-in-game (`crash_dump_format.cpp` `rva()`/`in_game` logic) |
| `thread_id` | text | Windows | `GetCurrentThreadId()` at capture, `crash_recovery.cpp:405` |
| `access_op` / `access_addr` | text | Windows, AV only | `crash_recovery.cpp:408-414` |
| `crash_module_name`/`base`/`end` | text | Windows, when attributable | module-table snapshot, `crash_recovery.cpp:466-474` (N70: never re-enumerated in-handler, always the init-time cache) |
| `stack_frames` | text (one line per frame, already formatted) | Windows | `crash_recovery.cpp:441-462`, up to 24 attributed RVAs |

Gzip-compressing the whole body is optional and safe to add later — not
required for v1 given payload sizes here are small (no Windows binary
minidump exists yet, see §6a).

## Auth

Bearer token in the `Authorization` header, generated the same way as the
existing `telemetry.token` (`telemetry_streamer.cpp:40-45` pattern). New
config keys `crash.uri` / `crash.token`, added to `service_map.cpp` the same
way `telemetry_uri`/`telemetry_token` were (lines 62-65) — not hardcoded.
Standard breakpad protocol itself defines no auth (research finding: relies
on HTTPS + a controlled/secret upload URL) — Bearer token is stricter than
the bare minimum but matches this codebase's existing idiom, so use it.

## Response contract

Minimal: `2xx` with `{"report_id": "<string>"}`. Client treats upload as
fire-and-forget from the deferred uploader (§ architecture) — on failure,
leave the spooled file in place and retry next launch/next background pass;
delete only after a `2xx`. This gives at-least-once delivery without needing
anything more sophisticated on either side.

## What the sink needs to do (Teth's implementation, not specified further here)

Accept the POST, check the bearer token, persist the raw multipart body +
parsed fields somewhere queryable, return the ack. Storage engine, retention,
symbolication, alerting, and dedup-by-`client_report_id` are entirely her
call — out of scope for this wire spec.

## Open decisions — not resolved here, need an answer before/while Teth builds

1. **Standalone service vs. a route on something Teth already runs.** I don't
   have visibility into her current infra layout; if a general ingest/events
   service already exists, a `/crash-reports` route there may be cheaper than
   a new service.
2. **Real Windows minidumps.** Today `WriteCrashDump` produces a structured
   *text* summary, not a binary `.dmp` — `MiniDumpWriteDump`/DbgHelp is not
   called anywhere in `crash_recovery.cpp`. Recommendation: ship v1 with
   `crash_summary_text` only on Windows (it already exists, is proven
   crash-safe, and needs zero new capture code) and treat a real binary
   minidump as separate follow-up work, not a blocker — adding DbgHelp as a
   dependency inside a VEH handler is its own safety review.
3. **Endpoint path/naming convention** — deferred to whatever Teth's infra
   already uses elsewhere, `/v1/crash-reports` above is a placeholder.

## Non-goals for v1

Symbolication pipeline, alerting/dashboard UI, retention policy — all
sink-side and Teth's to scope, not part of this wire-contract spec.

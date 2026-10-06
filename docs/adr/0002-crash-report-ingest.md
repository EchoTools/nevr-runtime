# ADR 0002: Crash reports reach the game service through a spool and a deferred uploader

Status: accepted, not implemented. No uploader exists in the runtime or the Quest
sentinel. The sink side is specified by `tests/crash-ingest/crash_ingest_contract_test.go`.

## Context

Neither platform sends a crash report anywhere. Windows prints a text summary
from `WriteCrashDump` (`src/runtime/lifecycle/crash_recovery.cpp`) to
`STD_ERROR_HANDLE` only, so a client that exits leaves nothing on disk to upload.
Quest's breakpad `DumpCallback` (`src/quest/sentinel/sentinel.cpp`) writes a
minidump and `/proc/self/maps` to local files.

## Decision

Capture writes to local disk from the fault context. A separate uploader, running
in an ordinary context, delivers from a spool directory. Delivery is at-least-once;
the sink makes it effectively once per `client_report_id`.

### Invariants

1. No DNS, TLS, HTTP, heap allocation, mutex, `Log()`, loader operation, or thread
   creation from Windows `BreakpointVEH` / `WriteCrashDump` or Quest `DumpCallback`.
2. Capture is durable before delivery is attempted.
3. A report is deleted only after an authenticated acknowledgement: status `200`
   or `201`, JSON content type, and a nonempty `report_id`. Network errors,
   malformed acknowledgements, `429` and `5xx` keep it.
4. Missing or invalid crash configuration disables delivery with one normal-context
   warning. It never blocks startup or changes crash recovery.
5. Reports go to HTTPS only, except `crash.allow_insecure_for_tests` in a
   non-release build. Tokens and raw bodies are never logged.

### Configuration

```yaml
crash:
  uri: https://staging.example/v1/crash-reports   # complete POST URL; no path appended, no redirects
  token: ${NEVR_CRASH_TOKEN:?}                    # opaque; sent as Authorization: Bearer
  allow_insecure_for_tests: false                 # rejected by release builds
```

`crash` joins the configuration top-level allowlist and is read through the typed
configuration route. A blank `uri` disables delivery; a `uri` with a blank `token`
is a configuration error and also disables it.

### Windows capture

At post-loader initialization, before the VEH is enabled, create a per-process
spool directory and pre-open three slot files with a UUID each. `WriteCrashDump`
keeps its stderr output and writes identical bytes to the next slot with fixed
buffers and raw `WriteFile`, then a commit footer (magic, byte count, checksum)
last. The uploader accepts only a valid footer and keeps partial slots for
diagnosis. Slot selection is atomic. When all slots are used, one raw stderr
warning is emitted and later reports are dropped; unacknowledged evidence is never
overwritten. Session ID and version are copied to fixed snapshots during ordinary
execution. The VEH capture filter is unchanged. Windows v1 sends text only.

### Quest capture

`DumpCallback` stays raw-I/O only and writes a completion marker last; a lone
`.dmp` is never eligible. The uploader persists `client_report_id` before the first
attempt. Upload does not start from the ELF constructor or `JNI_OnLoad`. It starts
from an explicit post-loader game lifecycle hook whose first action is a bounded
spool scan.

### Retry and retention

One uploader at a time, at most 32 reports or 256 MiB per scan, oldest first, a
15-second deadline, no redirects. Backoff is full-jitter exponential from 30 s to
6 h, honoring `Retry-After` on `429`/`503` up to 6 h. Permanent `400`, `401`, `403`
and `413` move to `rejected/` with a reason file. Rejected reports are kept 7 days
and unacknowledged reports 30 days, under a 512 MiB cap. Only the oldest rejected
reports are evicted; if only pending reports remain, capture stops.

### Sink

Authenticate before parsing, stream binary parts to private storage, commit
metadata and artifacts atomically before `201`, and enforce a restart- and
race-safe uniqueness constraint on `client_report_id`. Limits: 256 KiB summary,
32 MiB minidump, 8 MiB maps, 41 MiB total (enforced while streaming), 32 parts.
Field names, status codes and the acknowledgement shape are the test file's header.

## Acceptance

- `just test-crash-ingest-contract` against an isolated staging sink, never
  production.
- Runtime unit tests: footer validation, corrupt-slot rejection, slot exhaustion,
  UUID persistence, retry classification, `Retry-After` capping, no deletion on a
  malformed `2xx`, deletion only after a valid acknowledgement.
- Source-invariant checks rejecting networking, heap allocation, `Log` and thread
  creation in `WriteCrashDump`, `BreakpointVEH` and `DumpCallback`.
- A crash injected on a real run produces a committed record, the next safe pass
  uploads it exactly once, and the game starts and logs in afterwards.

## Open

- Which service hosts the sink (a route on an existing service or a new one).
  `crash.uri` is configuration, so the runtime does not depend on the answer.
- Windows binary minidumps, symbolication, alerting and dashboards are separate work.

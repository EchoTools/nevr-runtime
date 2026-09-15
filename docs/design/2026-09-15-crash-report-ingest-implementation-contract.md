# Crash-report ingest — implementation contract and acceptance gate

2026-09-15. This document replaces the unresolved parts of
`2026-09-15-crash-report-ingest-spec.md`. An implementation is not ready to
merge until every requirement below and the black-box conformance suite pass.

## Review outcome

The earlier wire spec has one fatal premise error: Windows does **not** write a
crash report to disk today. `WriteCrashDump` calls `VehPrintf`, which calls
`VehWrite`, which writes only to `STD_ERROR_HANDLE`. A deferred uploader cannot
upload stderr after a client process exits. It must not be implemented as a
network call in the VEH to compensate; that violates N70's crash-context rules.

The original document also left the endpoint path, accepted response, retry
policy, spool transaction, size limits, duplicate response, and Quest launch
context unspecified. Those are interoperability and data-loss decisions, not
implementation details.

## Non-negotiable invariants

1. No DNS, TLS, HTTP, heap allocation, mutex, `Log()`, loader operation, or
   thread creation occurs from Windows `BreakpointVEH` / `WriteCrashDump` or
   Quest `DumpCallback`.
2. Capture is durable before delivery is attempted. A client crash must survive
   to a later safe lifecycle pass.
3. Delete a report only after an authenticated, syntactically valid success
   acknowledgement. Preserve it for network errors, malformed acknowledgements,
   `429`, and all `5xx` responses.
4. Delivery is at-least-once. The sink makes it effectively once per
   `client_report_id`; a duplicate must not create a second stored report.
5. Missing or invalid crash configuration disables delivery with one normal
   context warning. It never blocks startup or changes crash recovery.
6. Crash artifacts are sensitive diagnostics. Send them only to HTTPS except
   for an explicit non-release test override; never log tokens or raw bodies.

## Fixed v1 HTTP contract

`crash.uri` is the complete HTTPS POST URL. The runtime must not append a path,
rewrite a scheme, follow redirects, or infer a host. A blank URI disables
delivery. A nonblank URI with a blank token is a configuration error and also
disables delivery. Test-only HTTP requires
`crash.allow_insecure_for_tests: true` in a non-release build.

Use `POST`, `Content-Type: multipart/form-data`, and exactly one
`Authorization: Bearer <crash.token>` header. Never put the token in a URL,
field, log, crash text, or retry file.

The only supported configuration shape is:

```yaml
crash:
  uri: https://staging.example/v1/crash-reports
  token: ${NEVR_CRASH_TOKEN:?}
  allow_insecure_for_tests: false
```

`crash` must be added to the configuration top-level allowlist and `uri` /
`token` must be accessed through the existing typed configuration route, not
ad-hoc environment reads. `token` is an opaque deployment secret; the runtime
does not generate it. `allow_insecure_for_tests` defaults to `false`, is
rejected by release builds, and must never appear in a distribution sample.

Every accepted request has these UTF-8 text fields:

| Field | Contract |
| --- | --- |
| `schema_version` | Exactly `1`. |
| `client_report_id` | Canonical lower-case RFC 4122 UUID; created once per captured report and retained across retries. |
| `platform` | Exactly `windows` or `quest`. |
| `prod` | Exactly `echovr`. |
| `ver` | Nonempty build/game version, at most 256 bytes. |
| `captured_at` | UTC RFC 3339 timestamp with `Z`, measured at capture rather than upload. |
| `crash_summary_text` | Nonempty UTF-8 diagnostic text, at most 256 KiB. Windows sends complete N70 text, not a subset. |

Omit optional fields rather than sending empty strings:
`session_id`, `exception_code`, `rip`, `location`, `thread_id`, `access_op`,
`access_addr`, `crash_module_name`, `crash_module_base`, `crash_module_end`, and
`stack_frames`.

`platform=quest` requires exactly one `upload_file_minidump`; it is one binary
part with a `.dmp` filename and `application/octet-stream`, maximum 32 MiB. It
is forbidden for `platform=windows`.
`upload_file_maps` is an optional Quest binary part with a `.maps` filename,
the same content type, and maximum 8 MiB. Windows v1 sends no binary dump.
Total request size is at most 41 MiB; enforce it while streaming, not after
buffering all bytes in memory.

Reject unknown fields, duplicate scalar/file fields, invalid UTF-8, a
platform/file mismatch, or more than 32 parts with `400`. Reject unauthenticated
requests with `401` or `403` before persistence. Enforce size limits with `413`
before persistence.

A new report replies `201 Created`; a duplicate `client_report_id` replies
`200 OK`. Both must be `application/json` with this semantic shape:

```json
{"report_id":"server-stable-id","duplicate":false}
```

For a duplicate, `report_id` equals the original identifier and `duplicate` is
`true`. `report_id` is nonempty and at most 256 bytes. The client deletes only
for `200` or `201` with a parseable JSON object, JSON content type, and a
nonempty `report_id`; a bare `2xx`, redirect, HTML body, or malformed JSON is
not an acknowledgement.

## Client capture and spool contract

### Windows

At ordinary post-loader initialization, before enabling the VEH, create a
per-process spool directory and pre-open three unique slot files. Generate each
slot UUID at this safe point. Directory creation, cleanup, UUID generation, and
handle allocation must never move into the VEH.

`WriteCrashDump` continues its existing stderr output and writes identical bytes
to its next pre-opened slot using fixed buffers and raw `WriteFile` only. It
writes a fixed commit footer last, containing magic, byte count, and checksum.
The uploader accepts only a matching valid footer. It retains partial/corrupt
slots for diagnosis but never uploads them. Slot selection is atomic; concurrent
faults cannot share a slot. When all slots are used, emit one raw stderr warning
and drop later reports—never overwrite unacknowledged evidence.

The existing VEH deliberately captures only its enumerated fatal codes whose
instruction pointer belongs to `echovr.exe` or `BugSplat64.dll`; v1 preserves
that filter. Session ID, version, and other non-crash-safe metadata are copied
to fixed immutable snapshots during ordinary execution. The VEH does not read
`std::string`, scan logs, or discover metadata while the process is compromised.

The ordinary-context uploader converts committed slot text and metadata to the
multipart request. It uses the HTTP library's multipart API, not hand-built
boundaries, and `nlohmann::json` for acknowledgement parsing. It must preserve
the slot UUID across retries.

### Quest

`DumpCallback` remains raw-I/O only. It leaves a fixed-format completion marker
beside the minidump/maps, written last. A lone `.dmp` is never eligible. The
safe uploader persists `client_report_id` before the first attempt and derives
`captured_at` from committed artifact metadata.

Do **not** start uploading from the ELF constructor or `JNI_OnLoad`: those are
loader-sensitive contexts. Add an explicit, measured post-loader game lifecycle
hook. Its first operation is a bounded spool scan, not network work on a game
hot path.

### Retry and retention

Run one uploader at a time; scan at most 32 reports or 256 MiB, oldest first.
Use a 15-second deadline and no redirects. Retry a report at most once per pass;
later passes use full-jitter exponential backoff from 30 seconds to six hours.
Honor valid `Retry-After` for `429`/`503`, capped at six hours. Move permanent
`400`, `401`, `403`, and `413` failures to `rejected/` with a reason file.
Retain rejected reports for seven days and unacknowledged reports for 30 days,
under a 512 MiB spool cap. Evict only oldest rejected reports; if only pending
reports remain, stop capture rather than delete evidence.

## Sink requirements

Authenticate before parse/persist work, stream binary parts to private storage,
and atomically commit metadata plus artifacts before returning `201`. Maintain
a restart- and race-safe uniqueness constraint on `client_report_id`. Duplicate
handling returns the original `report_id`; it never creates a new row. Do not
log multipart bodies, tokens, or dumps. Stored reports require production-log
access controls.

## Required tests and merge gate

Run the portable black-box suite against an isolated staging sink, never
production:

```sh
CRASH_INGEST_URL=https://staging.example/v1/crash-reports \
CRASH_INGEST_TOKEN=staging-secret \
just test-crash-ingest-contract
```

It verifies authentication rejection, both platform shapes, strict field
rejection, binary/platform validation, and duplicate acknowledgement. Sink
route unit tests alone are insufficient.

Runtime work also needs unit tests for footer validation, incomplete/corrupt
slot rejection, atomic slot exhaustion, UUID persistence, retry classification,
`Retry-After` capping, no deletion on malformed `2xx`, and deletion only after
a valid acknowledgement. Add source-invariant checks rejecting networking,
heap allocation, `Log`, and thread creation in `WriteCrashDump`, `BreakpointVEH`,
and `DumpCallback`. Before release, inject a crash: prove a committed record is
created, the next safe pass uploads it exactly once, and the game starts and
logs in afterwards. Run the normal runtime verification and post-commit login
smoke test too.

## Non-goals

Windows binary minidumps, server symbolication, alerting, dashboards, and any
change to crash recovery are separate work. They are not reasons to weaken the
capture or delivery guarantees above.

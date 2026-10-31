# ADR 0030: A player sends their log with one tap and gets a short code

Status: proposed. Nothing below is implemented. The service half needs a deploy that only the owner makes.
Tracking issue: #1788.

## Context

A player who reports a problem has to find a log file, know its path and answer questions about headset and version.
The log already holds all of that. The outcome is: on Quest and PC the player picks "Send log" in the game, the
runtime uploads the current session's log and the previous session's log (where a crash or bug usually is) to our
service, and shows a short code. The player posts the code in Discord and a maintainer fetches the log by it.

## What exists

- **The crash report upload** (`src/core/crash_send.h`) posts an allowlisted JSON body to nevr-api
  (`kCrashReportUrl`), with no credential, from the PC host (`src/runtime/host/crash_send_host.cpp`,
  `crash_send_winhttp.cpp`) and from the Quest sentinel (`src/quest/integration/crash_send_step.cpp`, through
  `src/quest/auth/curl_http.h`). The service route is `POST /v3/crash-reports` in `echovrce-web/backend`
  (`internal/handlers/crash_reports.go`): open, rate limited per IP, body capped at 32 KiB, stored in the Mongo
  collection `crash_reports`. There is no `nevr.crash_reports` table. #1609's remote-log upload was replaced by this
  path in #1701 (refs #1689).
- **The crash route cannot carry a log.** `models.CrashReportRequest` is a no-PII allowlist decoded with
  `DisallowUnknownFields`, and its comment forbids "a log line". A log upload is a new route with its own consent
  rule, not a new field on that one.
- **The Lounge's log codes** (e.g. `BQDSUVGW`, eight uppercase letters) are not made by anything in our repositories:
  a search of `nevr-runtime`, `echovrce-web`, `nakama`, `nevr-service` and `nevr-common` finds the string only in two
  bug notes that quote it. Whose service mints them is **undetermined**, so this design does not call that service and
  does not reuse its format. Our code is minted by our service.
- **The in-game seam on Quest** (`src/quest/sentinel/texture_menu.h`, `texture_menu.cpp`, `texture_menu_hook.cpp`)
  does not add a row. It repurposes the stock `graphics|msaa` row by rewriting the game's settings JSON before the game
  parses it, and it has no press event: a change of the row's index is observed in `ApplyRuntimeGfxSettings`
  (`HandleApply`). There is no row-adding facility in the tree.
- **The seam on PC:** no settings-row seam. The nearest are the inserted menu cards (`src/runtime/patch/menu_page.h`,
  `menu_cards.h`, #906) and the text writer (`src/runtime/patch/ui_text.h`, #896).
- **Log files.** PC: the run log written by `src/runtime/log/builtin_filter.cpp` and `nevr-boot.jsonl`
  (`src/runtime/log/boot_log_tee.cpp`) in the `logs` folder under the nEVR root (`nevr_root::LogsDir`). Quest:
  `nevr-sentinel.log` and its rotated files (`src/quest/sentinel/sentinel_log.cpp`) in the game's files folder.

## Decision

### Service (nevr-api, a deployed service: designed here, built and deployed by the owner)

- `POST /v3/player-logs` stores one upload and answers `201 {"code":"LOG-7K2Q"}`.
  Open route, like the crash route (a player may not be logged in), rate limited per IP, body capped at 1 MiB.
- `GET /v3/player-logs/{code}` returns the stored upload. Global Operators only (`OperatorMiddleware`), as
  `GET /v3/crash-reports` is.
- **The code.** `LOG-` plus 4 characters from the 32-symbol alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ` (no I, L, O, U),
  drawn from `crypto/rand`. The unique index on `code` rejects a collision and the handler draws again, up to 5 times,
  then answers 503. The code names a record; it is not a secret, because the read route needs an operator. 32^4 is
  about a million codes against a 30-day retention.
- **Retention.** A TTL index removes an upload 30 days after it was stored.
- **Body.** JSON: `schema` (1), `install_id` (the random id `crash_send.h` already keeps), `build`, `platform`
  (`pc`|`quest`), `headset`, and `files`: a list of `{name, session, truncated, text}` with at most 4 entries,
  `session` being `current` or `previous`. Unknown fields are a 400. The service records the source IP only in its
  request log, not in the document.
- **Request log.** One structured line per request at the handler: route, code, status, bytes, latency, install id.

- **Pasting the code elsewhere.** The player pastes the code into Discord and into the community bug-report form's
  log field, which feeds the maintainers' board. The code is therefore one token of plain ASCII with no whitespace, and
  it survives being typed, copied and re-cased: `GET /v3/player-logs/{code}` upper-cases the code and accepts it with or
  without the `LOG-` prefix. The in-game display shows the code as text the player can select or read aloud, never an
  image. A link form (the service front end's page for the code, behind the operator login) MAY be shown beside it; the
  link is a convenience for an operator who is already signed in and is not required to look a log up. The form and the
  board are the community's accounts: nothing in the runtime or the service calls them or knows their identifiers.

### Client (shared, `src/core/log_send.h`; written once for both binaries, #1598)

- Modelled on `crash_send.h`: a pure core (collect, bound, build the body with `nlohmann::json`, parse the answer) and
  an injected `post` function. PC: libcurl (`src/core/nevr_curl.h`, as `invite_transport_curl.cpp`). Quest:
  `curl_http.h` `PostJsonWithHeaders`. Certificates are verified, redirects are refused, as in the crash path.
- **Which files.** The current session's log and the previous session's log, nothing else: on PC the run log of this
  process and the newest earlier run log; on Quest `nevr-sentinel.log` and its first rotation. No minidump, no
  crash folder, no config, no credentials file, no directory listing. The file list is fixed in code, not read from
  the file system.
- **Size.** The last 256 KiB of each file; `truncated` says when the head was cut. The whole body is at most 1 MiB; a
  body that would exceed it drops the previous session's file first and says so in the answer to the player.
- **Consent.** Nothing is sent without the player's tap. There is no automatic or deferred upload. The confirmation
  text says that the log includes the player's platform account id (the login line logs the XPID, `docs/standards/logging.md`
  Rule 2).
- **Thread.** The POST runs on its own worker thread with a teardown path, never the game thread
  (`ApplyRuntimeGfxSettings` and the frame loop must not block).
- **Logging.** New tag `NEVR.LOGSEND`: one Info line per tap with the file names and sizes, one with the status and
  the code (never the log text), a Warning with the transport code on failure. The tag is added to the Subsystem Tags
  table in the same change.

### In-game UI (a separate step, needs reverse engineering first)

- **Quest.** Repurpose a second stock settings row the way Texture Resolution repurposes `graphics|msaa`: a
  two-value `STRING ARRAY`, where moving the index to the second value is the tap, and the code is shown by writing it
  into the row's label. Which stock row is free is an open question for the disassembly (all binaries, N of N), not decided here.
- **PC.** Open: an inserted menu card (#906) or a row; the same RE question.

### Open decisions for the owner

1. Service retention (30 days) and per-IP rate (0.2 requests/s, burst 5).
2. Redaction: whether to strip the XPID from the stored text on the service side. The default here is no redaction and
   an honest confirmation text.
3. Which stock settings row Quest repurposes, and which PC surface carries the control.

## Tests

- Core (GTest under Wine, and the Quest host test): the body parses as JSON, only the fixed files are read, a file
  larger than the cap is cut to its tail with `truncated`, a body over 1 MiB drops the previous file, the answer parser
  rejects a malformed code, and a fake `post` shows nothing is sent without the tap call.
- Service (Go): `POST` then `GET` round trip through a fake store, collision redraw, a lookup by lower-case code or by code without `LOG-` finds the same record, unknown field is a 400, body over
  1 MiB is a 413, `GET` without an operator is refused.
- A test that starts the real binary and makes one call, asserting the `NEVR.LOGSEND` record exists
  (`AGENTS.md`, logging item 6).

## Release

Not part of the stable that is built tonight. The client can merge once the service route is deployed; until then the
tap must show that the service has no route (an HTTP 404 is "service not ready", never a silent success).

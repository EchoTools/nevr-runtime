# Roadmap: server, client, Quest port — and crash reporting as the Quest-port starting point

2026-09-15, Claude + Andrew. This is a plan, not a fix — written so an
interruption only costs a `git log` read, not a re-explanation.

## The proposal (Andrew, verbatim intent)

1. Test that a nevr-runtime server works.
2. Test that a nevr-runtime client works.
3. Port nevr-runtime's features to Quest, one feature at a time, building a
   test-automation suite as we go. Acknowledged as "insanely complex" — two
   Quest 2 headsets available: Andrew's main unit, and a second dedicated to
   automated testing.
   - **3a, first feature: a crash handler.** Crash data should reach "the
     game service" so it's debuggable without physical access to a headset.

## Status of steps 1–2, as of this doc

**Step 1 (server) — NOT green.** `launch-server.sh` boots, logs in, joins the
social lobby, then hangs forever before `BeginMultiplayer`/dedicated-server
registration. Full trace, ruled-out hypotheses, and two live-diagnostic
confirmations that the block is upstream of the entire multiplayer-bringup
call chain: `docs/reference/server-mode-multiplayer-hang.md`. Not solved.
Current best lead (untried): find what enqueues the `CTaskTarget<SPhUpdateTriTask>`
task that eventually calls `BeginMultiplayer` — an indirectly-dispatched task-
scheduler call with no statically-resolvable caller (`revault_callers` returns
nothing for its wrapper functions).

**Step 2 (client) — confirmed working.** 2026-09-13: client connects
end-to-end to Andrew's custom server via the `pnsradmatchmaking.dll` patch +
reload fix (`MEMORY.md` project-facts entry). This session additionally fixed
two client-side regressions unrelated to the above (`launch-client.sh`
"Matchmaker unavailable", stale build + one-shot patch guard, commit
`b43925d`; then a port-collision on rapid relaunch, fixed by switching the
matchmaker bridge listener to ephemeral-port-with-retry, commit `6f64d09`).

So the honest state is: client work is ahead of server work. Step 1 is the
open blocker for calling the "server/client" gate done; it should keep
independent priority, not get displaced by step 3 starting.

## Step 3 — Quest port, scope note

Out of scope for this doc to plan feature-by-feature (that's a large,
evolving list). The one thing worth recording now: bring up the
test-automation suite *as the first feature ships*, not after several
features have landed — the second headset exists specifically so automated
runs don't compete with Andrew's own play sessions. `tests/quest/` already
exists (`elf_groundtruth_test.go`, a Go ground-truth test shelling to
`readelf`/`nm` against the built `.so` — BAC-1 through BAC-5 from the crash-
reporter design doc, see below) — extend that harness rather than starting a
second one.

## Step 3a — crash handler: what exists, what's missing, what's open

### What already exists (read before touching anything — Chesterton's fence)

**Windows side** — `src/runtime/lifecycle/crash_recovery.cpp` (1121 lines) +
`crash_dump_format.{h,cpp}`. `InstallCrashRecoveryHooks()` "suppress[es] the
BugSplat crash reporter and prevent[s] crash-triggered termination"
(`crash_recovery.h:5-7`) — this is the load-bearing fact behind treating
nevr-runtime as already sitting in BugSplat64.dll's position (it statically
replaces that DLL — the game's own crash reporter — entirely). The existing
system installs a VEH, does setjmp/longjmp recovery on null-pointer AVs in
server mode, and writes a **structured local log line**
(`FormatCrashExceptionSummary`, `crash_dump_format.h:11-13`) — allocation-
free, safe to call from exception context. **It does not transmit anything
over the network.** Nothing in `crash_recovery.{h,cpp}` or
`crash_dump_format.{h,cpp}` references HTTP/upload/POST/endpoint (grepped,
zero hits besides an unrelated comment mentioning "the HTTP API may still be
listening").

**Quest side** — `docs/design/2026-07-13-quest-crash-reporter-injection.md`
(Spritz, 2026-07-13) is a complete design: hijack `libovrplatformloader.so`
(loaded before any game code via `libr15.so`'s `DT_NEEDED` closure), arm a
vendored **breakpad** `ExceptionHandler` in a load-time constructor, forward
all real symbol resolution to the renamed original via Bionic group-scope
linking. Doc status line says "DESIGN COMPLETE, BUILD NOT YET VERIFIED ON
MAIN" as of 2026-08-01 — **since then it has actually been implemented**:
`src/quest/sentinel/{sentinel.h,sentinel.cpp,entry.cpp}` exist (commit
`3080428`, "feat(quest): breakpad crash-reporter libovrplatformloader.so",
plus a follow-up fix `955aa06`), with a `just build-android` /
`configure-android` / `test-android` recipe set (`justfile:75-96`) and the
Go ELF ground-truth test at `tests/quest/elf_groundtruth_test.go`. Per the
design doc, crash output is a real breakpad minidump written to
`/sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes/` (design
doc §3) — **local file only.** Grepped `sentinel.{h,cpp}` and `entry.cpp` for
http/upload/POST/network/send: zero hits. On-device verification (does the
constructor actually run before `libr15`, does group-scope symbol resolution
actually work for `libpnsovr`'s 170 `ovr_*` imports, does a real signal
produce a real minidump) is still unexecuted per the design doc's own
"Remains for on-device" list (§6) — this needs the physical Quest headset
Andrew now has available for testing.

### What's actually new: transmission to "the game service"

Neither platform sends a crash report anywhere today — both write locally
only. This is the actual net-new work for 3a.

**A transport already exists and is a strong candidate to reuse or mirror**:
`src/runtime/server/telemetry_streamer.{h,cpp}` (`TelemetryStreamer`)
maintains an authenticated WebSocket to a telemetry ingest service, with
Bearer-token auth on the upgrade request (`telemetry_streamer.h:52`) and a
protobuf wire format (`telemetry::v2::EchoArenaFrame`). Its endpoint is
resolved via `service_map.cpp:62-65` — config keys `telemetry.uri` /
`telemetry.token`, under a service the map comments call `nevr-stream`
(S4b), explicitly optional ("an absent uri/token is a correct/valid state").
This confirms "the game service" already has at least one working ingest
channel with auth — a crash channel is plausibly a sibling of this, not a
new service.

### Open questions — blocking implementation, not blocking this doc existing

1. **Which service, concretely, is "the game service" for crash reports?**
   Andrew's custom server (same one `nevr-stream`/telemetry already targets),
   Nakama, or something new. Determines whether this is a new `crash.uri`/
   `crash.token` config pair mirroring `service_map.cpp:62-65`, or reuse of
   the existing telemetry WS with a new frame type.
2. **Payload shape.** Real breakpad minidump + `/proc/self/maps` (Quest) and
   the existing structured local log line (Windows) are quite different
   artifacts. Does the service want raw minidump bytes, a symbolicated
   summary, or just enough correlation data (build id, timestamp, exception
   code) to cross-reference against the existing local JSONL logs that
   already exist on both platforms?
3. **Windows and Quest sharing one wire format vs. two.** Not yet decided.

None of these block writing code to *plumb* an existing local crash event out
to a network call — they block deciding the concrete shape of that call. Per
repo convention (see `service_config.cpp`, `service_map.cpp`), config keys
should be added the same way `telemetry.*` was, not hardcoded.

## Concrete next steps, in priority order

1. **Unblock step 1** — the server hang — independently of Quest work; it's
   the more direct dependency for "does the server work" being true. See
   `docs/reference/server-mode-multiplayer-hang.md` for the resume point.
2. **Answer the three open questions above with Andrew** before writing any
   transmission code — they're genuine dilemmas (his infra, his intent), not
   reversible guesses to make alone.
3. Once answered: add a `crash.uri`/`crash.token` (or equivalent) service-map
   entry, wire it into the existing local crash-capture call sites
   (`FormatCrashExceptionSummary` call site in `crash_recovery.cpp` on
   Windows; breakpad's minidump-callback in `src/quest/sentinel/sentinel.cpp`
   on Quest), and send.
4. On-device Quest verification (the design doc's unexecuted §6 items) can
   and should happen in parallel with (3) — it doesn't depend on the network
   question being settled, since it's testing local capture correctness
   first.

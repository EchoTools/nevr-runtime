# Manual test plan — everything nevr-runtime touches

**Purpose:** establish, by hand, what works and what does not across every feature
this runtime adds on top of `echovr.exe`. This is the release gate that automation
cannot cover: there is no automated client on any host, and the multiplayer
features cannot be exercised by one process.

**Companion:** `docs/process/smoke-tests.md` is the *automated* half — 56 checks
scored from a captured log by `tests/smoke/score-log.sh`. Run it first; it clears
the whole server-side boot path in 90 seconds and tells you which of the tests
below are already covered. This document covers what it cannot reach.

## Headcount

| Phase | People | Time | Why that many |
| --- | --- | --- | --- |
| **0 — server alone** | 1 (operator) | 15 min | No client needed. Mostly already automated. |
| **1 — client solo** | 1 | 45 min | One client against a live server. |
| **2 — identity & session, solo** | 1 | 30 min | Needs a real login and a real match join. |
| **3 — two-party** | **2** | 45 min | Friends, parties, VOIP and spectate each need a second human. |
| **4 — full match** | **8 + 1 spectator** | 60 min | 4v4 is the only way to test team mechanics, scoring, and load. The +1 tests the player/spectator identity collision. |

**Minimum to declare a release candidate: phases 0–3.** Phase 4 is the one that
finds what the others cannot, but it needs scheduling.

## How to read a test

Every row has a **bisect value** — what a failure *rules out*. That is the sort
key, not severity. A test that eliminates half the system on failure runs before a
test that only tells you about itself, even if the second one matters more to
players.

- **B5** — failure means nothing downstream is meaningful. Stop and fix.
- **B4** — failure isolates a whole subsystem (auth, networking, rendering).
- **B3** — failure isolates one feature, but that feature gates others.
- **B2** — failure isolates one feature.
- **B1** — cosmetic or informational; failure tells you about itself only.

**Ease** is E1 (trivial, one command) to E4 (needs coordination or setup).

Record every result. A test you skipped is not a test you passed.

---

## Phase 0 — Server alone (operator, 1 person)

Most of this is automated. Run the suite first and only hand-check what it
reports ABSENT.

```sh
./verify-server.sh phase0 default 90
./tests/smoke/score-log.sh /var/tmp/work-nevr-runtime/server-runs/phase0/server.log
```

Expected: **56 PASS, 0 FAIL.** Anything FAIL stops the release.

| # | Test | B | E | How | Pass |
| --- | --- | --- | --- | --- | --- |
| 0.1 | Server boots and registers | B5 | E1 | the command above | `NSLOBBY registered = 1`, exit code 0 |
| 0.2 | No window opens (headless) | B4 | E1 | same run | `max_game_windows = 0` |
| 0.3 | No crash dumps | B4 | E1 | same run | `crash_dumps_after` == `crash_dumps_before` |
| 0.4 | Graceful shutdown | B3 | E1 | same run | `Graceful shutdown initiated`, lobby unregisters |
| 0.5 | Port immediately re-bindable | B3 | E2 | after CTRL+C, start again at once | second run binds, no "address in use" |
| 0.6 | Plugin loads | B3 | E2 | stage `nevr_example.dll` into `echovr/bin/win10/plugins/` | `Loaded: … (API v3) caps=0x` |
| 0.7 | Loader refuses `log_filter.dll` | B2 | E2 | also stage `log_filter.dll` | `SKIPPED … superseded by the built-in log filter` |
| 0.8 | **UPnP port mapping** | B2 | E2 | config already has `"upnp":"true"` — just run | `[NEVR.UPNP] Port mapping added:`. **Was never broken — it was invisible** (N122): the success line was Debug-level while every failure path logged at Warning, so a working mapping and a UPnP that never ran produced identical output. Now Info. **Live confirmation still pending** — the two runs after the fix did not reach registration (service-side), so this line has not yet been observed on a registering run. First green run closes it. |
| 0.9 | Telemetry stream | B2 | E3 | add `telemetry_uri` + run a listener | `[NEVR.TELEMETRY] Connected` |
| 0.10 | Server survives 30+ min idle | B3 | E2 | `./verify-server.sh soak default 2000` | still registered, no crash, memory flat |
| 0.11 | Idle CPU is sane | B2 | E2 | `top -H -p $(pgrep -f echovr.exe)` during soak | **open question** — record the number, do not conclude from one run |

---

## Phase 1 — Client solo (1 person)

Launch: `./launch-client.sh 2>&1 | tee /var/tmp/work-nevr-runtime/client-p1.log`

Score it afterwards: `./tests/smoke/score-log.sh <log> client`

| # | Test | B | E | How | Pass |
| --- | --- | --- | --- | --- | --- |
| 1.1 | **Client launches at all** | **B5** | E1 | launch | game reaches main menu. Failure here voids every test below. |
| 1.2 | Windowed mode | B4 | E1 | same | a window appears; log shows `bit0_render=SET(WINDOWED)` |
| 1.3 | `-noovr` works without a headset | B4 | E1 | same | no VR init, no Oculus prompt |
| 1.4 | Plugin host flag says **client** | B3 | E1 | same log | `per-frame tick ALIVE … (host=client)`. `host=server` here is the N110 bug returning. |
| 1.5 | TLS to modern services | B4 | E1 | same log | `SSL/TLS moderni…`, and no cert errors |
| 1.6 | HTTP via curl bridge | B4 | E1 | same log | `[NEVR.HTTP] Response: 2xx`, no `curl failed` |
| 1.7 | No crash reporter window | B3 | E1 | force-quit the client | no BugSplat dialog |
| 1.8 | Client-mode errors are non-fatal | B3 | E2 | rename a module DLL, launch | warns and continues; **must not** hard-exit (server dies, client warns) |
| 1.9 | Log file written and rotates | B1 | E1 | check `echovr/bin/win10/logs/` | fresh `.jsonl` with this run's id |

---

## Phase 2 — Identity and session, solo (1 person)

This is the auth and identity path end to end. **Highest bisect value outside
"does it launch."**

| # | Test | B | E | How | Pass |
| --- | --- | --- | --- | --- | --- |
| 2.1 | **Fresh device-code login** | **B5** | E2 | delete cached credentials, launch | ASCII code box appears; completing it in a browser signs you in |
| 2.2 | Credentials persist | B4 | E1 | check for the credentials file after 2.1 | file written, no error |
| 2.3 | **Cached login does not re-prompt** | B4 | E1 | relaunch without deleting | `Using cached credentials` / `Loaded cached token`. **A second prompt is a FAIL even though login succeeds.** |
| 2.4 | Token refresh | B3 | E3 | hand-edit the cached expiry into the past, keep the refresh token, relaunch | `Token refreshed successfully`, no re-prompt |
| 2.5 | Password auth | B3 | E2 | configure password auth instead of device flow | logs in |
| 2.6 | Bearer token reaches the service | B4 | E1 | client log | `Attaching Bearer token`, **not** `Using URL credentials` |
| 2.7 | **Login accepted** | **B5** | E1 | client log | `[NEVR.WS] LOGIN SUCCESS` |
| 2.8 | XPID is DSC-prefixed | B3 | E1 | client log | `login injected xpid=DSC-…`, never `PSN-` or `UNK-` |
| 2.9 | Discord ID, no pnsovr | B3 | E2 | confirm no modified `pnsovr.dll` is deployed | logs in anyway; identity is the Discord ID |
| 2.10 | Join a server | **B5** | E2 | matchmake or direct-join a NEVR server | you are in a lobby, other slots visible |
| 2.11 | Loadout save round-trips | B2 | E2 | change a cosmetic, exit, relaunch | `[SAVE_SUCCESS]`, change persisted |
| 2.12 | Loadout reads back | B2 | E1 | same | `[CURRENT_LOADOUT] Player:` |
| 2.13 | **Cosmetic tints render** | B2 | E2 | equip a CDN-delivered tint | it is visibly applied on the player model — not merely `tints loaded` in the log |
| 2.14 | CDN cache populates | B1 | E1 | check `%LOCALAPPDATA%/EchoVR/cosmetics/v1/packages/` | `.evrp` files present |

---

## Phase 3 — Two people

**People: 2.** Both need working clients from phases 1–2. One of you should be on
a **tablet** if tablet parties are in scope.

**Person A = operator (you).** Runs the server, captures logs, drives the checks.
**Person B = second tester.** Needs only a client and instructions.

### Instructions for Person B

1. Launch the client and sign in.
2. Tell A your display name and account ID.
3. Do exactly what A asks, one step at a time, and say what you *see* — not what
   you think should have happened.
4. If something looks wrong, say so immediately and **do not close the client** —
   A needs the live state.

| # | Test | B | E | Who | Pass |
| --- | --- | --- | --- | --- | --- |
| 3.1 | **Both clients in one session** | **B5** | E2 | A+B | each sees the other's avatar and name |
| 3.2 | Friend request / accept | B3 | E2 | A→B | appears for B, accepting persists on both sides |
| 3.3 | Friends list populates | B3 | E1 | both | `FriendListSubscribeRequest sent`; each other visible |
| 3.4 | **Party formation** | B4 | E2 | A invites B | B joins A's party; both show as partied |
| 3.5 | Party stays together into a match | B4 | E3 | A+B queue as party | both land on the **same team, same server** |
| 3.6 | **Tablet party** | B3 | E3 | B on tablet | tablet client joins and holds the party |
| 3.7 | **VOIP works** | B4 | E2 | both talk | each hears the other. Wwise is blocked on servers — this proves VOIP survived that. |
| 3.8 | Spectate | B3 | E2 | B spectates A | B sees A's match live |
| 3.9 | **Player + own spectator bot** | B4 | E4 | A joins, A's bot spectates | **both present at once.** This is the identity-collision case the DSC-NOVR namespace exists for. If one kicks the other, that is the bug. |
| 3.10 | Ping / latency sane | B3 | E2 | both, in match | ping is stable and plausible; confirm the 100 ms addon fix holds |
| 3.11 | One client leaving is clean | B3 | E2 | B quits mid-session | A's session continues; server logs the removal, no crash |
| 3.12 | Server survives both leaving | B3 | E1 | both quit | server unregisters and exits cleanly, or holds for the next session |

---

## Phase 4 — Full match

**People: 8 players + 1 spectator = 9.** This is the only phase that tests team
mechanics, scoring, and real load.

### Instructions for all 8 players

1. Sign in and confirm your name is correct on your own nameplate.
2. Join the server the operator names. Do not matchmake unless told to.
3. Play a normal match to completion.
4. Afterwards, report: any rubber-banding, audio dropouts, name display problems,
   score errors, or moments the game felt wrong — with roughly *when*.
5. Do not close your client until the operator says so.

### Instructions for the spectator

1. Join as spectator, not player.
2. Confirm you can see all 8 players and both teams' scores.
3. If you are also one of the 8 on another account, confirm **both your presences
   coexist** (test 3.9 at scale).

| # | Test | B | E | Pass |
| --- | --- | --- | --- | --- |
| 4.1 | **8 players join one server** | **B5** | E4 | all 8 in, correct teams |
| 4.2 | Match starts and completes | **B5** | E3 | full round, clean end |
| 4.3 | Scoring correct | B4 | E2 | goals/saves/stats match what happened |
| 4.4 | Round timer and mercy rule | B3 | E2 | round-time and mercy-score overrides behave |
| 4.5 | Server stable under 8 | **B5** | E2 | no crash, no dump, no disconnects |
| 4.6 | Server CPU under load | B3 | E2 | measure during play — **this is the load test the idle measurement cannot substitute for** |
| 4.7 | All 8 nameplates correct | B3 | E1 | every name renders and **all eight differ** — this is the first real check of N123. Note any tofu/boxes (unicode); per the ReVault sweep the atlas depends on the viewer's UI language. |
| 4.8 | VOIP with 8 | B3 | E2 | no dropouts or robotic audio |
| 4.9 | Cosmetics visible cross-client | B2 | E2 | each player's tints render **on everyone else's screen** |
| 4.10 | Mid-match join/leave | B3 | E3 | a player leaving and rejoining is handled |
| 4.11 | Match end → next match | B3 | E3 | server accepts a second match without restart |
| 4.12 | Graceful shutdown with 8 connected | B4 | E2 | CTRL+C unregisters and exits; clients are told, not dropped silently |

---

## Not covered here, and why

| Feature | Status |
| --- | --- |
| Quest / standalone | `src/standalone/` is a stub and does not build. Nothing to test. |
| Broadcaster guard | `BroadcasterGuard::Install()` is an empty placeholder that logs `no-op placeholder, nothing installed`. |
| Build identity / plugin manifest at login | Not built (N112). `buildversion` is still the hardcoded literal `631547` and `publisher_lock` is empty in every login. |
| `displayname` from account | **Built 2026-07-30 (N123).** Sourced from the account's username via `TokenAuth_GetUsername`, falling back to the account id (unique) rather than a shared constant. **Unobserved** — needs a client run, so treat 4.7 as the confirmation. |
| Third-party game-mode plugins | The ABI exists at v3 with capability declaration, but no third-party plugin exists to test. Test 0.6 covers the loader, not the ecosystem. |

## Recording results

One line per test: `id | pass/fail/skip | who | what you saw`. A failure becomes a
ledger entry with its evidence rank per `docs/standards/verification.md`.

**Capture every client log**, not just failing ones — `2>&1 | tee <file>` — and
score them with `tests/smoke/score-log.sh <log> client`. A log from a run that
"seemed fine" is how you find the thing nobody noticed.

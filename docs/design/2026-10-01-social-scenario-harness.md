# Social scenario harness: one-client tests for friends and parties

2026-10-01. Status: built. The runtime's control endpoint (`src/runtime/scenario/`, compiled only by
the `mingw-scenario` preset and checked absent from release DLLs by
`tools/verify_scenario_control_absent.py`), the runner (`tools/scenario/`, `just scenario NAME`,
`just scenario-all`) and the YAML scenarios under `tools/scenario/scenarios/` implement it. The
endpoint is a loopback TCP listener, not the named pipe this design first described. The rest of this
document is the design and the owner's constraints.

## Why

On 2026-09-30 one party-invite test took the owner and a second tester about
two hours. The owner's own messages that night show where the time went:

| What a human did | Why | Really needed a human? |
| --- | --- | --- |
| Read the UI aloud (~15 times: "no oculus friends", "Invite to start party!", blank friend page, missing names, no "+") | The runtime did not log what the game decided; pnsrad's native log is off | No. Observability gap, since closed by the facade slot trace and the session-event trace |
| Found the invite blocker himself (`npe\|firstmatch\|completed`) | The game's invite-error event was not logged | No. Same gap, closed by `src/runtime/patch/party_invite_gate.cpp` |
| Clicked through the menus on the main display | Hand/mouse input does not reach menu buttons under Xephyr :101 | No. Missing harness |
| Installed each build on a second client and stayed online as the invite target | An invite was assumed to need a second online account | No. See below |
| Said when to restart the game | The game does not exit cleanly under Wine (#53) | No. Missing harness |
| Changed production account state (lockout, password) | Production account records | Partly: the cause was readable from Nakama's log |
| Deployed Nakama mid-test | Agents never deploy without his per-instance word | Yes, by rule |
| Decided: no pnsovr; override the first-match flag now, fix Nakama later | Design calls | Yes |

What the second tester contributed to the invite test: her account being
online (which made the "+" appear), and a second sample of the same sender-side
failure. Nothing from the receiving side: the invite never left the first
client. The two-client part of the test never happened.

## The finding that makes one client enough

Every fact that passes between two clients takes the same path:

    A's game -> A's bridge -> Nakama -> B's login session -> B's bridge -> B's game

Both ends cross code we own (`src/runtime/compat/ws_bridge.cpp`, which logs
every message in every frame, both directions, at Info on servers and Debug on
clients). The middle is in Nakama's log. So:

- Sending side: the game's action, then the request in our game->server frame log.
- Receiving side: the bridge injects the message Nakama would send into our own
  game's server->game stream, and the slot trace shows what the game does.
- "A friend is online" is one injected `FriendStatusNotify` (id, status 0).

## The owner's constraints (verbatim, 2026-10-01)

> "it just needs to do everything the way that it has been doing it via the
> launch_client.sh stuff. it needs to just verify the outbound ones are sent.
> it can even reply to them."

What these commit to:

- **Same launch path as `launch-client.sh`:** nested display :101, pristine
  install and restore, judged from the run's own log, real production login.
  No local Nakama, no separate rig, no second launcher. The harness extends
  `launch-client.sh`.
- **Outbound pass condition:** the request left the game. It appears in the
  game->server frame log with the right fields, and the bridge's send
  succeeded. Not: proving Nakama's handling or another client's receipt.
- **Replies:** the harness may inject the server's reply into our own game to
  drive the next step. Optional.

Where they stop:

- Outbound requests are **not** swallowed or blocked. They go to production as
  they do today.
- **No scenario sends a destructive request** (friend removal, kick, leave on a
  real party) until the owner rules on it. "Verify sent" does not cover changing
  a real friendship. Open question with the owner as of 2026-10-01.
- Production Nakama's log is a diagnostic, not part of the pass.

## Where a scenario enters the game

The 2026-09-30 invite bug decides this. The friend row's click handler
(`FUN_14018aa90` in echovr.exe) returned early on `npe|firstmatch|completed`
and dispatched `delegate_onpartyinviteerrorfirstmatchnotcompleted`. It never
called the facade.

- Calling the facade's SendInvite slot directly would have **passed** while the
  button was broken. Our slot sends `PartyInviteRequest` correctly.
- Calling the click handler would have **caught** it: no slot call, no request,
  an invite-error event.

Rules:

1. A feature scenario passes only through the game's own entry point, the one
   the human's click reaches.
2. Even the handler skips "is the control visible and enabled". Before firing
   it, the scenario asserts the slot results that make the control appear: the
   row exists, `FriendIsInvitable` = 1, the party is `Joinable`. (On
   2026-09-30 the "+" was missing for a while; a handler call would not have
   noticed.)
3. Calling a facade slot directly is only for localizing a failure (above or
   below the facade), never a pass. The facade's own logic is already covered
   without the game in `src/runtime/tests/test_social_facade.cpp`.

## Three scenarios, laid out the same way

Message names and hashes come from `src/runtime/compat/social_party.h` and
`src/runtime/compat/social_roster.h`, and from Nakama's
`server/evr/sns_friends.go` and `server/evr_pipeline_friends.go` (EchoTools
nakama).

**Invite (send).** Setup: inject a roster with friend F online
(`FriendListResponse` + `FriendStatusNotify(F, 0)`) and our own party
(`PartyCreateSuccess`). Assert F's row is invitable. Act: fire the row's
invite handler. Check: SendInvite slot called, then game->server
`PartyInviteRequest` (0xcf13f934540b5f5e) targeting F, or a named invite-error
session event.

**Party join (receive and accept).** Setup: roster with F online. Inject:
`PartyInviteNotify` {PartyID P, InviterID F} (0x218f721f09026dab). Check the
invite slots report one invite from F, plus the invite session event. Act:
accept through the game's own handler. Check: game->server `PartyJoinRequest`
(0xb57b22cc5352e00c) carrying P. Reply: inject `PartyJoinSuccess`
(0xb57a32de4552e00b) with members [F, self]. Check: joined and member-joined
events, member slots name F, party size 2. Failure branch: inject
`PartyJoinFailure` and check the failure is reported with no member change.

**Friend removal (both directions).** Destructive outbound: blocked until the
owner rules (above). Incoming side is safe: inject `SNSFriendRemoveNotify(F)`
and check `FriendCount` drops by one, F's row is gone and no stale name is
cached, with no request sent. Outgoing side, once allowed: act through the
friends UI's remove handler, check `SNSFriendRemoveRequest`
(0x78908988b7fe6db4) with F's id, inject `SNSFriendRemoveResponse` and check
the same roster change.

## What is the same in every scenario

1. **Bring the client up and down.** Install the build, launch on :101, wait
   for "logged in" and the social accessor, and on exit clear the prefix (the
   #53 exit hang).
2. **Put the game in a state.** Inject server->game messages.
3. **Make the game act.** Fire a game entry point, or deliver a message.
4. **Assert on the run's own log.** An ordered list of expected lines with
   timeouts: frame log (by message name), facade slot trace (slot, args,
   result), session events.

Only the message names, fields, the entry point and the expected lines differ.

## What the harness consists of

- **A control endpoint in the runtime**, compiled in test builds only behind a
  CMake option and absent from release DLLs, because it can inject messages
  into a live session. A loopback TCP listener on an OS-assigned port, with three verbs:
  - inject (message name + fields as JSON, encoded by the runtime's existing
    wire builders so the format is the code's, not the test's)
  - fire (a game entry point, such as a UI handler; facade slots only in a
    diagnostic mode that cannot produce a pass)
  - state (roster and party snapshot as JSON)
- **A runner that extends `launch-client.sh`:** install, launch nested, wait for
  logged in, play a scenario, check expectations against the run's log, tear
  down, print PASS/FAIL with the failing expectation. Exit 1 on fail; no
  `|| true`.
- **Scenario files** (YAML): setup injections, the action, expected log lines
  in order with timeouts. A new feature test is a new file of about 20 lines and
  a run of a couple of minutes, with no human.
- **Unit replay:** each scenario's message sequence can also be fed to the
  party and roster state machines in `src/runtime/tests/test_social_facade.cpp`
  for a loop that needs no game.

## State when this document was written

- The invite fix (first-match override plus event trace,
  `src/runtime/patch/party_invite_gate.cpp`, installed from the social
  accessor) had not been exercised in a client run; the first slice below did that.
- Facade: `src/runtime/patch/social_facade_object.cpp`. Party state machine and
  wire builders: `src/runtime/compat/social_party.h`. Roster:
  `src/runtime/compat/social_roster.h`.

## First slice, landed 2026-10-01

- Control endpoint: `src/runtime/scenario/scenario_control.cpp` (TCP on 127.0.0.1,
  ephemeral port logged as "[NEVR.SCENARIO] control listening on ..."), protocol in
  `src/runtime/scenario/scenario_protocol.h`. Ops: state, inject FriendStatusNotify,
  fire friend_invite. CMake option NEVR_SCENARIO_CONTROL, preset mingw-scenario.
  `tools/verify_scenario_control_absent.py` runs in `just verify` against the release DLL.
- "fire friend_invite" does what the friend row's script node (echovr.exe 0x140dddf60)
  does: SNSUserID on the row's id string, then posts the invite handler 0x14018aa90 on
  the NetGame deferred queue. It enters below the widget and above everything else.
- Runner: `tools/scenario/run_scenario.py`; scenario: `tools/scenario/scenarios/invite.yaml`;
  `just scenario invite`. Run folders under /var/tmp/work-nevr-runtime/scenario-runs/.
- What the first runs found: the handler dropped every invite silently on its provider
  check. pnsrad's UserProviderID reported "RAD", which the game maps to code 0, while
  every "OVR-ORG-" id maps to 4. Fixed by making that export return "OVR"
  (`src/runtime/patch/provider_identity.h`). Before the fix the scenario failed at
  "game called the facade's SendInvite slot"; after it, all eight steps pass.


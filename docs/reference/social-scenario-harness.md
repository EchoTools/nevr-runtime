# Social scenario harness

One client, no human: a scenario brings the game up on the nested display, puts it in a state by injecting
the messages the server would send, makes it act through the game's own entry point, and judges the run's own
log. `just scenario NAME` runs one scenario and `just scenario-all` runs every scenario in turn and prints one
PASS/FAIL table. The reasons for the shape (the owner's constraints and the failure that decided it) are in
`docs/design/2026-10-01-social-scenario-harness.md`.

## Why one client is enough

Every fact that passes between two clients takes the same path:

    A's game -> A's bridge -> Nakama -> B's login session -> B's bridge -> B's game

Both ends cross code the runtime owns (`src/runtime/compat/ws_bridge.cpp` logs every message in every frame,
both directions, at Info on servers and Debug on clients). So:

- Sending side: the game's action, then the request in the game->server frame log.
- Receiving side: the bridge injects the message Nakama would send into the game's own server->game stream, and
  the slot trace shows what the game does.
- "A friend is online" is one injected `FriendStatusNotify` (id, status 0).

## What a pass is

- Same launch path as `launch-client.sh`: nested display `:101`, pristine install and restore, judged from the
  run's own log, real production login. No local Nakama, no second launcher.
- Outbound pass condition: the request left the game. It appears in the game->server frame log with the right
  fields and the bridge's send succeeded. Proving Nakama's handling or another client's receipt is not part of
  it. Outbound requests are not swallowed; they go to production.
- Replies: a scenario may inject the server's reply into the game to drive the next step.
- No scenario sends a destructive request (friend removal, kick, leave on a real party) until the owner rules
  on it.
- Production Nakama's log is a diagnostic, not part of the pass.

## Where a scenario enters the game

1. A feature scenario passes only through the game's own entry point, the one a human's click reaches (a friend
   row's invite handler, `FUN_14018aa90` in `echovr.exe`).
2. Before it fires the handler the scenario asserts the slot results that make the control appear: the row
   exists, `FriendIsInvitable` is 1, the party is `Joinable`.
3. Calling a facade slot directly localizes a failure (above or below the facade) and is never a pass: a slot
   call passes while a broken click handler drops the action. The facade's own logic is covered without the game
   in `src/runtime/tests/test_social_facade.cpp`.

`fire friend_invite` does what the friend row's script node (`echovr.exe` `0x140dddf60`) does: `SNSUserID` on
the row's id string, then posts the invite handler `0x14018aa90` on the NetGame deferred queue. It enters below
the widget and above everything else.

## Parts

- **Control endpoint** (`src/runtime/scenario/scenario_control.cpp`, protocol in
  `src/runtime/scenario/scenario_protocol.h`): a loopback TCP listener on `127.0.0.1` with an OS-assigned
  port, logged as `[NEVR.SCENARIO] control listening on ...`. One JSON object per line each way. It is compiled
  only by the `mingw-scenario` preset (CMake option `NEVR_SCENARIO_CONTROL`), because it can inject messages
  into a live session, and `tools/verify_scenario_control_absent.py` checks the release DLL for its absence
  in `just verify`. The protocol header is pure, so every build's unit tests cover it.
- **Verbs**: inject (a server->game message by name and fields, encoded by the runtime's existing wire
  builders, so the format is the code's), fire (a game entry point; facade slots only in a diagnostic mode that
  cannot produce a pass), state (the roster and party snapshot as JSON).
- **Runner** (`tools/scenario/run_scenario.py`, `tools/scenario/run_all.py`, `tools/scenario/control.py`):
  install, launch nested, wait for "logged in" and the social accessor, play the scenario, check the expected
  log lines against the run's log, tear down (clearing the prefix, because the client does not always exit),
  print PASS/FAIL with the failing expectation. It exits 1 on a failure. Run folders are under
  `/var/tmp/work-nevr-runtime/scenario-runs/`.
- **Scenarios** (`tools/scenario/scenarios/*.yaml`): setup injections, the action, and the expected log lines
  in order with timeouts: frame log (by message name), facade slot trace (slot, args, result), session events.
  A new feature test is a new file of about twenty lines.
- **Unit replay**: a scenario's message sequence can also be fed to the party and roster state machines in
  `src/runtime/tests/test_social_facade.cpp`.

## What is the same in every scenario

1. Bring the client up and down.
2. Put the game in a state by injecting server->game messages.
3. Make the game act: fire a game entry point or deliver a message.
4. Assert on the run's own log.

Only the message names, fields, entry point and expected lines differ.

## Message sources

Message names and hashes come from `src/runtime/compat/social_party.h` and `src/runtime/compat/social_roster.h`
and from Nakama's `server/evr/sns_friends.go` and `server/evr_pipeline_friends.go`. Examples:
`PartyInviteRequest` `0xcf13f934540b5f5e`, `PartyInviteNotify` `0x218f721f09026dab`, `PartyJoinRequest`
`0xb57b22cc5352e00c`, `PartyJoinSuccess` `0xb57a32de4552e00b`, `SNSFriendRemoveRequest` `0x78908988b7fe6db4`.

## Findings the first scenario produced

The invite handler dropped every invite silently on its provider check: pnsrad's `UserProviderID` reported
"RAD", which the game maps to code 0, while every "OVR-ORG-" id maps to 4. The export now returns "OVR"
(`src/runtime/patch/provider_identity.h`). The observability the harness depends on is the facade slot trace and
the session-event trace, and the game's invite-error event is logged by `src/runtime/patch/party_invite_gate.cpp`.

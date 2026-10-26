# ADR 0017: invite codes for the CREATE GAME / JOIN VIA CODE wizard are issued by the game service and carried by a runtime hook

Status: proposed (#906). Items marked **Decision needed** are not settled by this record.

The key words "MUST", "MUST NOT", "SHOULD", "MAY" are used as in `AGENTS.md`.

## Context

The final game builds contain a wizard, "deep linking" in the game's own metric names, that no service backs:
main-menu cards CREATE GAME and JOIN VIA CODE, then the `mnu_master` pages 34 to 41 (a code entry page, a link
page, experience, arena mode, combat mode, map, settings, share). Its research is the work of Dualgame's
`echovr-modkit` (`rad-links`, MIT) and, for a service, of Dualgame's `EchoRelay` fork. No code from either is
copied by this record; the ids and behaviours below are facts, each re-checked in ReVault (grades as in
`docs/standards/verification.md`: VERIFIED, STATIC, UNVERIFIED).

What the game does, measured in ReVault over all 1,041 binaries of project `echovr`:

- **It sends nothing a service can answer.** A page script logs one click metric per button: panel type
  `deep_linking` (`0x75705bbdb2a10bd7`, `CSymbol64("deep_linking")`), an event name (`create_invite`,
  `join_via_code`) and up to four string pairs (`game_type`, `Password`, `Server_region`, `Expires_after` on the
  create page; `join_code` on the entry page). The scripts that carry the flows hold no URL, route or message name.
  The metric reaches the service as `SNSRemoteLogSetv3` (`echovr.exe` `0x140113960`, only when remote logging is on).
  STATIC.
- **It expects nothing back.** No reply message is read. The code, the expiry and the link text are text actors the
  stock scripts never fill; something in the process has to write them (modkit: `CCanvasUICS::SetText` on each
  actor, the code script refusing outside calls). UNVERIFIED here.
- **A join must start in the client.** `CR15NetClientLobby::LobbySessionSuccessCB` (`echovr.exe` `0x14017db90`)
  returns at once unless the lobby state is 4, 5 or 6, so a `LobbySessionSuccess` pushed to a client at the main
  menu is dropped (VERIFIED against the nakama comment on the same behaviour). The client starts a join in one of two
  ways: the `-lobbyid` block staged in `CR15Game` and read by `BeginMultiplayer` (the function PLAY runs; the runtime
  already binds it as `BeginMultiplayerLobby`, base+`0xFDDD0`), or `CR15NetClientLobby::Join(gametype, lobby, team)`
  (`echovr.exe` `0x140178700`, Quest `0x13462a8`), which sends `SNSLobbyJoinSessionRequest`.
- **`DeepLinkCB` is not the wizard.** `CR15NetGame::DeepLinkCB` (Quest `0x136efa0`; PC `PartyDeepLinkCB`
  `0x14015fe60`) is the delegate for the Oculus platform's launch-details deeplink (`CNSOVRSocial::FollowDeepLink`).
  `SR15DeeplinkingSettings` has one function, an `operator==`, in `libr15.so`.

What the game service has (nakama):

- A private game is allocated by `LobbyGameServerAllocate` with `MatchSettings` (mode, level, owner, group,
  `ReservationLifetime`); the Discord create command does it today (`evr_discord_appbot_handlers.go`).
- Join by match id is `lobbyJoin(matchID)`, which re-checks the joiner against the lobby's guild (`lobbyAuthorize`).
- Named RPCs are registered in a table with a permission (`evr_runtime_rpc_registration.go`, for example
  `party/create` with `RequireAuth`), state lives in storage collections owned by the system user (`Parties`,
  `match_reservations`), and `RemoteLogMetrics` is on by default in the game settings sent at login.
- Nothing issues, stores or resolves an invite code. The only occurrence of the words is an error constant,
  `ErrCodeInviteCodeWasEitherInvalidOrTaken`.

What the runtime has: libcurl calls to nakama RPCs with the player's token (`token_auth`, `auth_refresh`), a
per-frame dispatcher on the game thread (N86), `PatchDetour` with HookGuard (N84, N126), prologue validation (N97)
and a binding for `BeginMultiplayerLobby`. It has no accessor for the wizard's text actors; that is the UI work in #896.

## Decision

### 1. The service issues the codes

Nakama issues, stores, resolves and revokes the codes, as RPCs and storage in its own idiom. The runtime never
issues a code and never builds a route shaped after the wizard.

| RPC | Request | Answer |
| --- | --- | --- |
| `invite/create` | experience, mode, map, region, requested expiry in seconds | code, expiry time, match id |
| `invite/resolve` | code | match id, mode, level; or a refusal |
| `invite/revoke` | code | ok |

A refusal is a nakama RPC error whose message is one stable token: `invalid` (no such code), `expired`, `closed` (revoked,
or its match ended), `denied` (the guild check, decision D2), `limit` (decision D1, live codes per user) or `rate` (too many
attempts). The runtime maps the token to the wizard's text and logs the token; it never logs or shows any other text of
the answer (`docs/standards/logging.md` Rule 4).

- **Storage.** One object per code in a system-owned collection, key = the code, value = match id, owner user id,
  group id, expiry, revoked flag. A code is an alias for a match id and is not derived from it.
- **Why an alias.** The match id is a 36-character UUID and the entry field holds a bounded string (decision D1 below
  gives the bound and what is not known). A code the player can type on a controller and read aloud must be short.
- **Lifetime.** A code ends at its expiry, when its owner revokes it, or when its match ends, whichever is first. A
  dead code answers "closed" or "expired", never "invalid", so the wizard can say which.
- **Private game.** `invite/create` allocates through the same path as the Discord create command and marks the
  match private so the matchmaker does not backfill it.
- **Attempts.** `invite/resolve` is rate limited per user and per source address; a miss costs the caller more than a hit.

#### Decisions (service)

- **D1. Code format and lifetime.**
  - *Alphabet:* `A`-`Z` and `2`-`9` without the look-alikes `0 O 1 I L`: 23 letters and 8 digits, 31 symbols. Players
    read a code aloud and type it on a controller. The runtime upper-cases what was typed and removes spaces and
    hyphens before it checks the alphabet.
  - *Length:* the length the wizard's entry field holds, **N, pending a headset/PC measurement**. It is not invented.
    What was tried: `entry_max_length` and `entry_min_length` occur only in the entry page's script
    (`c9cb2299a1b7abe7.dll` and its Quest twin; 2 hits in 1,041 of 1,041 binaries). The script copies them from the
    instance's variable table into int32 slots (`FUN_1800026a0`, `+0x28` and `+0x2c`; set through
    `script_set_variable` on the Quest twin) and holds no literal, so the values are level-resource data that code
    search cannot reach (UNVERIFIED). The text container is a 64-byte string, copied up to 63 characters
    (`487df6d160c8da42.dll` `FUN_180008170`; STATIC), so 63 is the upper bound whatever the field says. The measurement
    step (read the instance's two values from the level resource of a copy of the install, or type a long code on a
    running client) is in the live-run plan; the service's code length is set to N once it is measured, and the client
    enforces only the alphabet and the 63-character bound.
  - *Expiry:* 24 hours by default, 7 days at most. Scrims are set up the day before.
  - *Live codes:* at most 5 per user, so a code cannot be used to flood storage. The sixth `invite/create` answers `limit`.
- **D2. A code never bypasses the guild check.** `invite/resolve` answers with the match; the join then runs
  `lobbyAuthorize` as for any join by id, and a player outside the lobby's guild is refused (`denied`). A code is a way
  to find a match, not permission to join it.
- **D3. No passwords in the first release.** The wizard's password field stays unused, so the runtime neither reads nor
  forwards it. If passwords are ever added, the create metric MUST be checked for the field before that ships, because
  nakama journals remote logs.

### 2. A runtime hook carries the wizard to the service

The runtime is the only side that can see the click, write the text and start the join, so the client half is part of
the runtime (`src/runtime/patch/`), not a service configuration and not a plugin that installs its own detours
(#896: no plugin installs a detour of its own).

- **See the click.** Two observation points, each a detour recorded by HookGuard through `PatchDetour` and
  prologue-validated before it is written, each forwarding every call unchanged and doing no wait and no network
  call on the game's thread:
  - the remote-log submit (`echovr.exe` `0x140113960`) for the create and join metrics. The metric line is JSON; as
    modkit reports it, the line holds `"panel_type":"deep_linking"`, `panel_name`, `event_name` and `user_id`;
    the create metric (`create_game_review_settings` / `create_invite`) holds `Server_region` and `Expires_after`
    (text such as `3 HOURS`, `30 MINUTES`, `1 DAY`); the join metric (`join_via_code` / `join`) holds `join_code`
    (`<invalid>` when the field is invalid). UNVERIFIED until a capture. Frequency: the submit has one caller,
    `CR15NetJsonLog::WriteLog`, so it runs per metric event, not per frame (STATIC; the rate under play is measured
    before the hook ships).
  - the component-event send (`echovr.exe` `0x14050fb60`, `CncaGameSpace::SendComponentEvent`) for the wizard's card
    choices (experience, mode, map), which the metric does not carry reliably. It is called from many sites, so the
    hook rejects every event that is not a card or button event on one of the wizard's eight page owners before it
    does anything else; its call rate under play is measured before the hook ships.
- **Call the service.** The hook queues a request; a worker thread posts the RPC with the token the runtime already
  holds and libcurl; the answer is applied from the per-frame dispatcher (N86), so no game memory is touched from the
  worker.
- **Show the result.** On a created code, the dispatcher writes the code, `EXPIRES: HH:MM UTC` and the share text into
  the share page's actors; on a refusal it writes the reason into the entry field's title. This needs the UI
  accessors of #896 (a typed set of a text actor); until they exist the first release shows the result in the log only.
- **Join.** On a resolved code, from the main menu the dispatcher stages the `-lobbyid` block and runs
  `BeginMultiplayerLobby`; from a lobby or match it calls `CR15NetClientLobby::Join`. Both prologues are validated
  at boot (N97); on a mismatch the feature turns itself off and says why. It is a client feature: on a dedicated
  server (`-server`) nothing is installed.
- **Where it is configured.** A `config.yaml` key under the existing sections turns it on (N133); there is no
  environment variable and no hard-coded host.

Rejected:

- **Service only, no hook.** Nakama sees the click (the metric is sent by default) and could allocate the match, but
  it cannot write the code into the client and cannot start a join (see Context: the pushed `LobbySessionSuccess`
  is dropped at the main menu). The wizard's own falsifier is not met: no existing route answers it.
- **Code = match id.** Does not fit the entry field.
- **Codes over the game's websocket (a new message).** Needs a new game message symbol on both sides for what the RPC
  table already does, and the runtime already speaks RPC.
- **Delivery of the code out of band (a Discord message).** Works without a text accessor but moves the experience out
  of the game. It stays available as a fallback and is not in the first release.

**Decision needed (D4).** Whether the runtime package ships the main-menu cards that make the wizard reachable. In modkit they are
language-package data; the stock menu may not show them. This is a game-data authoring and packaging question.

### 3. Platforms

The service contract is platform-neutral. PC is built first (the PC addresses and prologues above). Quest has the
same scripts and the same `Join` body (`libr15.so`, ReVault virtual address `0x13462a8`) and gets its own hook in
`src/quest`, through that project's own mechanism, against the same RPCs.

## Logging

Every line has the tag `[NEVR.INVITE]` (a new row in `docs/standards/logging.md`, added with the first code) and
follows `docs/standards/logging.md` Rules 1, 4, 8, 9 and 10. A code is a bearer secret and a password is a
credential: a line carries `code_len` and a short fingerprint of the code (`code_fp`), never the text.

| Event | Level | Fields |
| --- | --- | --- |
| wizard click seen | Info | `event=create_invite\|join_via_code panel=deep_linking` |
| request not sent | Info | `reason=empty\|malformed\|not_configured\|no_token` and `code_len=<n>` (nothing is sent) |
| request sent | Info | `op=create\|resolve\|revoke expires_s=<n> code_len=<n> code_fp=<8 hex>` |
| answer | Info / Warning | `op status=<http status> latency_ms=<n> result=<ok\|invalid\|expired\|closed\|denied\|limit\|rate>` |
| text written | Info | `page=<n> field=<name>`; a failed write at Warning with the cause |
| join started | Info | `route=stage\|join lobby=<uuid> state_from=<n>`; the state change after it is the game's own `[NETGAME]` line |
| feature off | Info | `reason=` (a prologue differed, no token, not configured) |

The service writes one structured record per RPC (principal, session, client, source address; operation with its full
arguments and the resolved match id and user id; outcome, latency, result size) at the single point every RPC passes,
which the RPC table's wrapper already is. A request that is refused is logged with its reason. The nakama issue for
this feature carries that requirement.

## Order of work

1. This record.
2. The nakama issue (service half): the three RPCs, the storage collection, the rate limit, the per-request record.
3. A host test of the client half: the metric parser, the code normaliser and alphabet check, the expiry mapping,
   the request builder, the answer-token mapping and the log lines, written to fail first, with a mutant for each
   changed line. No game and no network. The module is not yet wired into the runtime.
4. The hook, the worker and the dispatcher step behind the config key, with the PC login test on the tip.
5. A live run on a copy of the install: create a code on one client, join with it from another.
6. The Quest hook, after the UI accessors of #896.

## Not shown

- That the typed code and the create settings are in the metric as sent. Script strings and the modkit report say so;
  no packet has been captured.
- The code field's length limit N (level data; the measurement step is in the live-run plan).
- Whether the wizard's expiry choices are the three texts modkit reports; the client treats an unrecognised text as
  "no request" for expiry and lets the service apply its 24-hour default.
- That the runtime's token is accepted by the RPC route as a user session. The token authenticates the game
  connection (`SelectRemoteBearer`); the RPC call with it is not measured.
- The argument layout of the submit function `0x140113960`. It is measured before the hook is written, and the hook
  validates the prologue bytes it records.
- Anything on Quest beyond the static reading above.

## Relation to other records

- ADR 0008 (BugSplat64.dll host, runtime): the feature is part of the runtime the host loads.
- #896: the UI accessors the text writes and the Quest hook wait for.

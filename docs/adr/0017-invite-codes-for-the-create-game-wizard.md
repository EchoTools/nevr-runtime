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
or its match ended), `denied` (the guild check, decision D2), `limit` (decision D1, live codes per user), `rate` (too many
attempts) or `unavailable` (the match could not be allocated: no server was available to the player). The create request's
`map` is a level name string (the service refuses a number as `invalid`). The runtime names it from the chosen map card:
the card's level symbol is looked up in the generated symbol cache (`symcache_data.cpp`), giving `mpl_combat_fission` and
`mpl_combat_gauss` for the payload cards and `mpl_combat_dyson` and `mpl_combat_combustion` for the capture-point cards.
A card with no level, or a level the cache cannot name, is refused with the `no_level` reason and nothing is sent; the
runtime never sends a create with no level for a mode that has a map page. A mode with no map page sends an empty `map`.
The request's `region` is empty: the review
page's `Server_region` is display text, no generic region code exists, and with none sent the service picks a server by
the player's stored pings (#1192 makes the row a list of codes the service offers, and the request then sends the chosen
one). The runtime maps the token to the wizard's text and logs the token; it never logs or shows any other text of
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
    does anything else: it reads the event symbol (the first word the game itself reads) and goes on only for the
    six wizard event symbols, then resolves the sender to an actor name through checked reads
    (`patch/invite_choices.h`, `patch/invite_choice_hook.cpp`); its call rate under play is measured before the hook
    ships. Every wizard event on a wizard page logs at Info with the page, the card and the choices so far, and a
    running count of the hook's calls (`seen=`).
- **Call the service.** The hook queues a request; a worker thread posts the RPC with the token the runtime already
  holds and libcurl; the answer is applied from the per-frame dispatcher (N86), so no game memory is touched from the
  worker.
- **Show the result.** Each frame, on the game-loop thread, `nevr_invite_apply::OnFrame` takes the answers the worker holds
  and posts their texts to the UI text writer (#896; `patch/invite_answer_text.h` is the rule). A created code: the code and
  `EXPIRES: HH:MM UTC` in the share page's code and expiry elements, and `<coming soon>` in its link element (the stock link
  text is a dead domain; #1726 turns it into the code's link). A refused create, or an `ok` whose body holds no code
  the client may show: the reason where the code would be, the expiry cleared. A refused resolve: the reason in the entry
  page's title. A resolved code writes nothing; the join below does the rest. The texts are never logged, only the field and the size.
- **Join.** On a resolved code, from the main menu the dispatcher stages the `-lobbyid` block and runs
  `BeginMultiplayerLobby`; from a lobby or match it calls `CR15NetClientLobby::Join`. Both prologues are validated
  at boot (N97); on a mismatch that hook is not installed and its line says why. The service starts before the hooks and
  stays on when one refuses (#1414). It is a client feature: on a dedicated
  server (`-server`) nothing is installed.
- **Where it is configured.** The `network.invite_codes` key in `config.yaml` (N133) is on by default for a client: an
  absent or empty key is on, and `false` (or `off`, `no`, `0`) turns it off. Any other value, a list or a map included, is
  off with a warning, so a value that is not a switch word never turns it on. A key the runtime does not read (a
  misspelling, or a `config.yaml` it rejects) is as if absent, and so is on. The startup line says `source=default` or
  `source=config`. There is no environment variable and no hard-coded host.

Rejected:

- **Service only, no hook.** Nakama sees the click (the metric is sent by default) and could allocate the match, but
  it cannot write the code into the client and cannot start a join (see Context: the pushed `LobbySessionSuccess`
  is dropped at the main menu). The wizard's own falsifier is not met: no existing route answers it.
- **Code = match id.** Does not fit the entry field.
- **Codes over the game's websocket (a new message).** Needs a new game message symbol on both sides for what the RPC
  table already does, and the runtime already speaks RPC.
- **Delivery of the code out of band (a Discord message).** Works without a text accessor but moves the experience out
  of the game. It stays available as a fallback and is not in the first release.

#### Decision (client): the package ships the main-menu cards (D4)

**D4. The runtime package ships the CREATE GAME and JOIN VIA CODE cards on the main menu.** Without them the stock
menu has no way into the wizard, so a code feature that is not reachable is not shipped. The choice is reversible: it
is data, it can be turned off or changed once players have used it ("accommodating first, adjust later").

How echovr-modkit (MIT, by Dualgame) ships them, from its `mods/menu-cards/README.md` and `mods/menu-cards/menu_cards.py`
(facts only; no code is copied here):

- The cards are data, not code. The main-menu page resource (`CR15UIPage2CR`, package manifest type id `0x4a3fe67c1ff9ccc4`, name `mnu_master`,
  `0xac360e41e4ede056`, PC; page 24 inside it) is read from the player's own install, two card elements are inserted
  (CREATE GAME, owner `0xff5375e5a4dee29d`; JOIN VIA CODE, owner `0x097b785350a61c5f`, each with a sprite of the stock card
  canvas), and the result is appended as one row to a copy of the language package (`2b47aab238f60515`) in
  `_data/5932408047/rad15/win10`. The engine loads that package after the base package (`48037dc70b0ecab2`) and the
  later row wins.
- Every stock input is checked against a sha256 pin first (base manifest, language manifest and package, the page, the
  element records and the card canvas); another game version, or a package already modified, is refused. The PC pins
  are for build `6323983201049540`; the Quest pins are for build `6528386917200980`. Cards exist for en_us only.
- `apply` writes the copy into the install and `revert` puts the stock pair back from a kept copy. A loose-file form of
  the same page is also written (`--loose`) for a loader that serves single resources.

What in this tree carries the page: `RegisterResourceOverrideFromFile` and `RegisterResourceOverride` in
`src/runtime/patch/resource_override.cpp` replace a resource by its type and name symbols in the game's resource-load
callback, so a patched page can be served for (`0x75e5993f80940cd0`, the class symbol of `CR15UIPage2CR` that the callback reports,
and `0xac360e41e4ede056`) without writing into the install.

**The package never carries Ready At Dawn's data.** The package carries only our work; Ready At Dawn's data stays
theirs. The main-menu page is therefore built on the player's machine from the player's own install:

- **In memory, at runtime.** When the resource-load callback delivers the stock page, the runtime checks its sha256
  against the pin, inserts the two card elements into a copy of those bytes and serves the copy. Nothing is written to
  the install and nothing derived from the data is stored or shipped. This is the way.
- **A file derived from the install, only if in-memory cannot work.** The fallback is a build step that reads the
  player's install, writes the patched page outside the install, and serves it from there. It is taken only when the
  in-memory path is shown not to work on the build we ship, and the change that takes it records why in this record. It
  still ships no stock bytes.

Unverified, and measured before the cards are built: that the callback offers the whole original page at that point
(the size, and whether the bytes are final), that a served replacement of this page is applied on the build we ship
against, and that the card elements fit when inserted into a buffer the callback owns. A failure of any of these is the
reason the fallback records.

Consequences for the package:

- The package carries the card elements as our own data (the element records and the pins), never the stock page or
  a page derived from it.
- The pins make the cards build-specific: on any other game build the cards are off and the log says why.
- The order of the four main-menu buttons (Play, Join, Create, Tutorial from the left) is the order of the slots in the
  level's transform table (`CTransformCR`, the same bytes on PC and Quest), not of the page's element array: the game
  stands each card where its owner actor's transform says. `runtime/patch/menu_slots.h` gives the four owners the
  poses of the four slots in that order, behind its own pin (`kStockTransformSha256`), and both platforms call it through
  `PlaceButtons` (`runtime/patch/menu_cards.h`). Each run logs the slot of each button, read back from the bytes served.
- The cards are English only until another language is built.
- A change to the package contents (a pin table) is a change to what a release carries and goes through the
  release checks.
- Quest takes the same data through its own path (modkit writes to the data folder on the headset, outside the APK);
  that waits for the Quest hook and keeps the same rule: no Ready At Dawn data in the package.

### 3. Platforms

The service contract is platform-neutral. PC is built first (the PC addresses and prologues above). Quest has the
same scripts and the same `Join` body (`libr15.so`, ReVault virtual address `0x13462a8`) and gets its own hook in
`src/quest`, through that project's own mechanism, against the same RPCs.

The wizard's texts are written by one writer on both binaries (`patch/ui_text.h`: the queue, the actor lookup, the write, the
log lines). Only the address table and the memory it reads differ. On the Quest (`quest/invite/ui_text_quest.cpp`) the game
object is the frame hook's netgame +8; `CncaGame::GameSpace(index)` (libr15 `0x17e17fc`; the PC's `GetGameSpace`, the same
function with the array at `+0x1f48` and the count at `+0x1f78`), `CLocalComponentHandle<CCanvasUICS>::Find` (`0x1cbb408`) and
`CCanvasUICS::SetText` (`0x139111c`) are each pinned by their prologue words (`sentinel/pinned_targets.h`; ReVault lists each
`0x100000` higher). The Quest's actor table differs from the PC's: `[gs+0x370]` is a name-sorted array of 16-byte entries (a
signed 64-bit name, the actor index as a u16 at `+8`, the count at `table+0x30`), read as `CncaGameSpace::LocalActor` reads it
(`nevr_ui_text::FindActorInSortedMap`). The answers reach it through `nevr_invite_match::TakeShareText`, filed by the same take
that files a resolved match.

## Security

On a PC client, `network.invite_codes` is on by default (`src/runtime/patch/invite_config.h:17-18`, `:27`): an absent or
empty key is on. A `config.yaml` the runtime rejects is read as an empty config, so the key is absent and the feature is on
(`src/runtime/lifecycle/service_config.cpp:159-170`; `src/runtime/lifecycle/boot.cpp:687-689`). In that case the client sends
its signed-in token with each invite RPC to `nevr_http_uri`, which falls back to the embedded service address
(`src/runtime/lifecycle/service_config.cpp:199`). A plain `http://` address is refused unless its host is loopback
(`src/runtime/patch/invite_transport.h:92-94`). The service starts on a client only (`src/runtime/lifecycle/boot.cpp:681`).
To turn the feature off, set `network.invite_codes: false` explicitly: the start line then logs
`[NEVR.INVITE] feature off reason=turned_off source=config` (`src/runtime/patch/invite_service.cpp:71-72`, `:119`). A value
that is not a switch word is off, not on (`src/runtime/patch/invite_config.h:4-6`).

## Logging

Every line has the tag `[NEVR.INVITE]` (a new row in `docs/standards/logging.md`, added with the first code) and
follows `docs/standards/logging.md` Rules 1, 4, 8, 9 and 10. A code is a bearer secret and a password is a
credential: a line carries `code_len` and a short fingerprint of the code (`code_fp`), never the text.

| Event | Level | Fields |
| --- | --- | --- |
| wizard click seen | Info | `event=create_invite\|join_via_code panel=deep_linking` |
| request not sent | Info | `reason=empty\|malformed\|too_long\|incomplete\|not_configured\|no_token\|busy\|stopped` and `code_len=<n>` (nothing is sent; `not_configured` is any reason the feature is off, which the start line gives) |
| request sent | Info | `op=create\|resolve\|revoke expires_s=<n> code_len=<n> code_fp=<8 hex>` |
| answer | Info / Warning | `op status=<http status> latency_ms=<n> result=<ok\|invalid\|expired\|closed\|denied\|limit\|rate>` |
| worker stopped | Info | `dropped=<n>`: requests still queued when the worker stopped, never sent |
| answers dropped | Warning | `reason=not_taken kept=<n>`: the oldest answer went because the menu had not taken them |
| text posted | Info | `field=code\|expires\|entry_title bytes=<n>`; a post the writer refused, or a writer that is off, is `text not posted` at Warning with `reason=`; the writer's own `[NEVR.UI] text written` / `text dropped` line follows |
| join started | Info | `route=stage\|join lobby=<uuid> state_from=<n>`; the state change after it is the game's own `[NETGAME]` line |
| service on | Info | `source=default\|config`: where the value came from (the default when the key is absent or empty) |
| feature off | Info / Warning | `reason=turned_off` (Info: the key says false), `unrecognised_value`, `no_service_address` or `service_address_refused` (Warning), each with `source=default\|config`; a hook's `prologue_mismatch` is that hook's own line, without `source=` |

The service writes one structured record per RPC (principal, session, client, source address; operation with its full
arguments and the resolved match id and user id; outcome, latency, result size) at the single point every RPC passes,
which the RPC table's wrapper already is. A request that is refused is logged with its reason. The nakama issue for
this feature carries that requirement.

## Order of work

1. This record.
2. The nakama issue (service half): the three RPCs, the storage collection, the rate limit, the per-request record.
3. A host test of the client half: the metric parser, the code normaliser and alphabet check, the expiry mapping,
   the request builder, the answer-token mapping and the log lines, written to fail first, with a mutant for each
   changed line. No game and no network. The module is not yet wired into the runtime; step 4 wires it (#906).
4. The hook, the worker and the dispatcher step behind the config key, with the PC login test run on main after the release cut.
5. A live run on a copy of the install: create a code on one client, join with it from another.
6. The Quest text writes and the Quest submit, after the UI accessors of #896 and the invite transport. The Quest observer lands before them.

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
- That the share page's code, expiry and link actors and elements (the modkit author's findings) are the ones the shipped build
  draws, from a write made by this tree: a wrong id is a request the writer drops with `reason=no_actor` or `no_canvas`.
- Anything on Quest beyond the static reading above.

## Quest observer on MetricLog (#966)

The Quest half of the wizard observer hooks the game's `NRadEngine::NRadGame::NR15Logger::MetricLog(char const*, unsigned long long)` in `libr15.so` through a GOT slot. Addresses are ReVault virtual addresses unless marked pinned. A pinned address is the ReVault address minus `0x100000`, the libr15 offset: `0x130cc50` is pinned `0x120cc50`. The pins are in `src/quest/sentinel/pinned_targets.h`.

Measured:

- `revault_search_code` for `MetricLog`, scope `searched all 1,041 binaries in project echovr`: 3 hits, all in `libr15.so`: `MetricLog` at `0x130cc50`, its thunk at `0x10057c0`, and `CR15NetJsonLog::WriteLog` at `0x136a304`, the only decompiled caller.
- `revault_search_code` for `deep_linking`, same scope: 5 hits. `echovr.exe` `GetMetricsPanelName` (`0x140f4b4c0`) maps the panel hash `0x75705bbdb2a10bd7` to the name, and `libr15.so` `Enter` (`0x2428098`, `0x24288a4`) compares the same hash.
- Call sites in the store `libr15.so` (`aarch64-linux-gnu-objdump -d`): one `bl` to the MetricLog PLT stub `0xf057c0`, at pinned `0x126a4cc`, inside `WriteLog` (entered at pinned `0x126a304`). No `b` to the stub.
- Relocation (`readelf -rW`): one `R_AARCH64_JUMP_SLOT` names the symbol, at pinned slot `0x36da2c8`, with addend `0x120cc50`. `readelf -sW --dyn-syms` gives the symbol's value as `0x120cc50` (FUNC, GLOBAL, DEFAULT).
- Prologue: the first four instruction words at pinned `0x120cc50` are `a9bc63f9 a9015bf7 a90253f5 a9037bf3`, the `stp` pairs that save the callee-saved registers. The observer compares these 16 bytes before it writes anything.
- The store file's SHA-256 is the pin in the `test-quest-hooks-pinned` recipe.

Arguments: `x0` is the JSON text and `x1` is its length. The prologue keeps both (`mov x19, x1` at pinned `0x120cc88`, `mov x20, x0` at pinned `0x120cc8c`).

Why MetricLog and not the submit: `NRadEngine::SNSRemoteLogSetv3::Send` (`libr15.so` `0x130bf00`, raw decompilation) copies its `char const*` argument into a 16-byte field, with the copy length capped at `0x10` (`if (0xf < uVar2) uVar7 = 0x10`). A metric longer than that does not reach it whole. `MetricLog` receives the whole text and its length.

Observer behaviour (`src/quest/invite/wizard_metric_hook.cpp`):

- It copies the text first. The copy is a pipe write, so the kernel reads the game's buffer and an unmapped byte is refused with `EFAULT` instead of faulting the game's thread. Lengths from 1 to `nevr_invite::kMetricMaxBytes` (4096) are copied; other lengths and a null pointer are passed on unread.
- It runs the PC's shared parser from `src/runtime/patch/invite_wizard.h`, unchanged: `MayBeWizardMetric`, `ParseMetric`, `PlanRequest`.
- It logs `invite_click` with `click` (`create_invite` or `join_via_code`) and `panel=deep_linking`. A planned request that is not sent logs `invite_request_not_sent` with `reason`, `op` and `code_len`. No line carries the code text.
- It calls the original once, with the arguments it was given, after its own work. The observer returns before the game's function runs.
- The submit is off: `kSubmitEnabled` is `false`, so a planned request is logged with `reason=observe_only` and nothing is sent. Until the invite transport is on main, the submit path only counts plans.
- A create click carries no card choices (the choices observer is a later slice), so it logs `reason=incomplete`.

The click's field is `click`, not the PC's `event=`. The sentinel's JSON record already names each line with `event` (`"event":"invite_click"`), and a second `event` key would be read as the record's name. The value is the same as the PC's.

Install:

- The sequence step `install_wizard_metric` (`src/quest/integration/ctor_sequence.cpp`) runs after the menu-cards step, and only when `WizardMetricWanted()` is true. That is `sentinel::FeatureEnabled(Feature::kInviteCodes)`, the `network.invite_codes` key (`src/quest/sentinel/quest_config.cpp`), which is off by default and needs login and a service address. With the key off the step is skipped with `reason=feature_off`.
- The install checks the 16 prologue bytes at image base plus `0x120cc50` before any write. A mismatch logs `invite_observer_off` with `reason=prologue_mismatch`, `va`, `expected` and `actual`, and arms nothing. A refusal from the GOT step logs its status name under the same event.
- A successful install logs `invite_observer_installed` with `target=MetricLog` and `slot=0x0000000036da2c8`.

Frequency: the hook runs once per `MetricLog` call, and `WriteLog` makes one call per metric event. The observer counts entries and copies (`calls`, `copies`) and logs no per-call line. The call rate under play is not measured.

Tests:

- `src/quest/tests/wizard_metric_hook_test.cpp` (`just test-quest-hooks`): the observer against a stand-in for the game's function. It covers the create and join lines, unusable codes, an escaped panel name, null, zero and oversize lengths, an unreadable page, the prologue check, the install refusals, and the ten skip tokens.
- `src/quest/tests/got_pinned_test.cpp` (`just test-quest-hooks-pinned`): the store file's JUMP_SLOT, its value and the prologue bytes.
- `src/quest/tests/integration_sequence_test.cpp` (`just test-quest-integration`): the step's order and its gate.

Not shown:

- That a Quest click reaches `MetricLog` as this JSON, with the event and the panel. No capture exists; the test lines use the PC's metric shapes.
- Whether remote logging, when it is off, returns before `MetricLog` and drops the metric. Not measured here.
- Whether the code and the create settings are in that JSON as sent. No packet capture exists.
- The call rate and the behaviour on a headset. No headset run has been made.
- The Quest text writes. They need the UI accessors of #896; this observer writes no text.

Prediction, written before any Quest run: with remote logging on, a click on CREATE GAME or JOIN VIA CODE produces one `invite_click` line from the observer. A click that produces none falsifies `MetricLog` as the hook point for the wizard's metric; the next candidate is the call chain above `WriteLog`.

## Quest join on a resolved code (#966, slice S4)

The Quest's JOIN VIA CODE ends in the same staged join the PC uses (`src/runtime/patch/invite_join.h`), with libr15's offsets. The glue is `src/quest/invite/join_game_quest.cpp`; the host test is `src/quest/tests/invite_join_quest_test.cpp` (`just test-quest-hooks`).

Measured in ReVault (project `echovr`, binary `libr15.so`; ReVault = pinned + `0x100000`):

- `CR15Game::BeginMultiplayer(SR15NetMatchSettings const&)` at `0x1301d88`, decompiled. It reads the netgame at `this+0x8510`, the route byte at `+0x8518` (`& 0xf`), the gate qword at `+0x8520`, the lobby `SUuid` at `+0x8528` (against the invalid id), the staged `SUuid` at `+0x8538` and the team `u16` at `+0x8558`. Its listing has no `#0x8560` operand, so the gametype word is neither read nor written. Its exit stores the route to `+0x8518`, zeroes `+0x8520` and sets `+0x8528`, `+0x8538` and `+0x8558` back to the invalid id and `0xffff`. Comment `grade A, S0 2026-10-29`.
- `CR15Game::BeginMultiplayer()` at `0x1301be4`: 52 bytes, its first 16 bytes `ff 83 01 d1 ff fb 04 a9 68 00 c0 d2 09 00 80 92` (`revault_read_memory`), calling the settings body through `thunk 0xfdc040`. Pinned `0x1201be4`.
- `CR15Game::EnableTransitionMenu()` at `0x13005fc` (the plan's "mp_transition"): its first 16 bytes `ff 03 01 d1 f3 7b 03 a9 13 78 7d f9 d3 01 00 b4`. It sends one component event to the menu gamespace at `+0x7af0`. Pinned `0x12005fc`.

The join's pins are in `src/quest/sentinel/pinned_targets.h` (`kBeginMultiplayerVaddr`, `kEnableTransitionMenuVaddr`) and are checked against the store file in `got_pinned_test.cpp`.

Write path, on the game thread only, through the frame dispatcher (`src/quest/invite/frame_dispatch.cpp`):

- The netgame the frame hook passes is `self` of `HookedNetGameUpdate`. Its state is at `netgame+0` (`game_layout::kStateOffset`), and its game object (CR15Game) at `netgame+8` (`game_layout::kGameOffset`).
- The route is read first. Nothing is staged when the route byte and the gate are both set, or when `+0x8528` is not the invalid id (`other_route`).
- Then the 16-byte GUID is written at `+0x8538` and `0xFFFF` at `+0x8558`, both read back. A mismatch puts the prior bytes back.
- Then `EnableTransitionMenu(game)` and `BeginMultiplayer(game)`, once each, with a 15 s window that refuses a repeat as `busy`. This is the order the Quest code uses, and it is not measured on Quest. The PC join calls only the begin (`src/runtime/patch/invite_join_game.cpp`, `Begin`: `EchoVR::BeginMultiplayerLobby(game)` and nothing before it), so the two joins do not share a menu step.
- Only `state == 3` (`LoggedIn`, `src/abi/echovr.h:361`) joins. Any other state logs `not_at_menu`.
- The answers reach the join through `nevr_invite_match::TakeResolvedMatch` (`src/runtime/patch/invite_match_api.h`), a non-throwing call implemented in `invite_service.cpp`. The frame applier never includes the worker or the service header, so the hook-side translation unit is exception-free.

Prediction, written before any headset run: with the feature on and a resolved answer for a match id that is in progress on the server, the log shows `join started route=stage lobby=<GUID> state_from=3`, then the game's own `[NETGAME]` state change. A join whose state is not 3 logs `join not started reason=not_at_menu`. A stage that does not read back logs `reason=stage_failed` and writes nothing that stays.

Not shown:

- That the Quest's `BeginMultiplayer` joins the match when called this way. No headset run has been made.
- That the game's netgame state at `+0` is 3 at the menu on a headset. The value is the PC's enum; the Quest's use of it is the existing login-prompt pin (`game_layout::kStateOffset`).
- The Quest's `+0x8520` gate semantics beyond the decompile comment. The 281-instruction listing was not re-read in this slice.
- The route check's invalid-id value on Quest: it is the PC's sixteen zero bytes, not separately verified in libr15.
- Any run against the production service (U4, the invite RPC with the Quest token).
- Whether the menu event before the begin is what the game does. Not measured on Quest; the headset run is owed. The reviewed ReVault reading (not re-measured here) found `EnableTransitionMenu` called by `JoinLobby` (libr15 `0x139682c`) after `Join`, by `SwitchTo` (`0x135b8b4`) right after `FindLobby`, and not by the two `BeginMultiplayer` callers, so the Quest order has no ReVault support yet.

## Relation to other records

- ADR 0008 (BugSplat64.dll host, runtime): the feature is part of the runtime the host loads.
- #896: the UI accessors the Quest text writes wait for. The Quest observer does not wait for it.

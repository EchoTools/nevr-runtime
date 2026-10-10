# Social features: the Nakama messages the runtime consumes

The runtime replaces the game's own social layer (pnsovr) with a facade, and Nakama supplies what pnsovr read
from Oculus. This is the built part of that protocol: the capability field, the friend presence notify, recently
met, party data and the join-policy request. What is not built (the presence publish, enforcement of a join
policy, the invitable-users button) is the approved design in
`docs/design/2026-10-01-social-nakama-proposal.md`. Paths: Nakama's server is `server/...` in the
`EchoTools/nakama` repository; the runtime is this repository. A claim not read in code is marked
**UNVERIFIED**.

## Who reads these messages

Friend and party frames are parsed by the bridge (`src/runtime/compat/social_party.h` `Feed`,
`src/runtime/compat/social_roster.h` `ParseStatusNotify` / `ParseListResponse`) and drive the facade's slots
(`src/runtime/patch/social_facade_object.cpp`). The bridge also forwards every frame to the game; what the game
does with a social symbol it does not know is **UNVERIFIED**.

The bridge's parsers check minimum lengths only (`len >= 8/16` in `Feed`, `len < 17` in `ParseStatusNotify`),
so bytes appended to an existing server-to-client message are ignored by old clients. Nakama's decoder requires
the frame length to match (`ParsePacket` in `server/evr/core_packet.go`), so new client data travels in new
message types, not in widened requests.

## Capability, not build number

`LoginProfile` (`server/evr/login_request.go`) carries the game's build, not the runtime's. The bridge writes
the login JSON itself (`src/runtime/compat/ws_bridge.cpp`) and adds `"nevr_social": <level>`; Nakama keeps it in
the session parameters and sends the messages below only to sessions that declared a level that knows them.
Level 1 is everything on this page.

## Adding a message

Nakama: `SymbolTypes` in `server/evr/core_packet.go`, `NewMessageFromHash` in `server/evr/core_packet_types.go`,
the message type in `server/evr/types.go`, and a `case *evr.X:` in `server/evr_pipeline.go`; the symbol is
`ToSymbol(Token())`. The runtime adds the symbol to `social_party.h` (request constants and `ReplyTable`).
Check each new token's hash against `server/evr/core_hash_lookup.go` for a collision first.

## A friend's party and status text

The game reads slots 52 `FriendStatusString`, 54 `FriendIsJoinable` and 55 `FriendPartyId`; pnsovr filled them
from each friend's Oculus presence, and the game cuts the status text at the first `|` (`FUN_140da5d80`).

New server-to-client `SNSFriendPresenceNotify`: Header u64, FriendID u64, PartyID u64, Joinable u8, Status u8
(online, busy, offline, as `FriendStatusNotify`), Reserved [6], `text_len u16`, text. Nakama sends it once per
friend in the friend-list refresh after the existing `SNSFriendStatusNotify` (kept for old clients) and on every
followed status change. `PartyID` is 0 and `Joinable` 0 when the friend is in no party or the party would refuse
this viewer. The roster gains party id, joinable and status text per friend, and slots 52/54/55 return them.
The scenario `friend_presence` injects it and asserts the slots.

## Recently met

The game reads slots 56-67 (count 58, online count 59, id 61, name and status string 62/64, status 63,
invitable 65, joinable 66, party id 67) and refreshes through `R15NetRefreshRecentlyMetUsersNode` (`0x140ddfcc0`
-> `0x14019b870` -> slot 57), polling slot 56 until it returns 0.

- Who counts as met: at a player's leave, everyone in the match's `participations` (everyone who ever joined)
  whose time there overlapped theirs, so both sides of a meeting record it whoever leaves first; moderators
  (invisible) are never met; private and social lobbies included. Written to the `RecentlyMet` collection, key
  `list` (owner read, no client write) off the match loop, retried once on a version conflict; Nakama logs
  "Recently met recorded".
- Blocks either way are left out at write and again at read.
- Messages: `SNSRecentlyMetRefreshRequest` (the standard 0x20 header) when slot 57 runs;
  `SNSRecentlyMetListResponse`: Count u32, then per entry AccountID u64, PartyID u64, Joinable u8, Status u8,
  Reserved [6], `name_len u16` + name, `text_len u16` + status text.
- Slot contracts (pnsovr `0x180091200..0x180090b80`): slot 56 is busy from slot 57 until the answer, and the
  facade also ends it after 5 s because a server without the message never answers; slot 63 is 2 for the online
  prefix, else 0; slot 65 needs the local party joinable (slot 22), the person online and not a member.
- Scenario `recently_met` fires the refresh node (`refresh_recently_met`), asserts the request leaves, injects a
  response with two entries and asserts count, ids, names and status through state and the game's
  `R15NetRecentlyMetUser` expression.

## Party and member data

The host writes the party JSON (`social+0x1f0`, via `0x14015fdb0`, which also sets flags bit 0) and every member
writes its own member JSON (slot 30, `+0x248 + idx*16`). Keys come from scripts, so they cannot be listed from
the executable. The runtime fills keys the game reads in C++: `headsettype` per member (read at `0x14018a090`
and in `PartyMemberJoinedCB`).

- `SNSPartyDataUpdateRequest` (`0x3448ca6e8d9dd0ce`): the standard 0x28 header with TargetParam = scope (0
  party, 1 member), then `seqid u32`, `json_len u32` and a JSON object of at most 4 KiB. Party scope from the
  leader only (else `SNSPartyUpdateFailure` code 2); member scope from the sender for itself. The reply is
  `SNSPartyUpdateSuccess` / `SNSPartyUpdateMemberSuccess`. A write whose seqid is not newer than the stored one
  from the same session is acknowledged and not kept.
- `SNSPartyDataNotify` (`0x832143ccbf160955`), server to client: PartyID(8), MemberID(8, 0 = the party's
  data), seqid(4), json_len(4), JSON. Only level-1 sessions receive it.
- The server fills `lobbyid` (upper-case match GUID, or the nil GUID), `matchtype` (the mode symbol, or -1),
  `team` (or 65535), `lobbytype` (or 2) and `offline` for the writer, plus `headsettype` (1 Rift, 2 Rift S, 3
  Quest, 4 Quest on PC, else 0, from the login profile) for a member, over whatever the client sent under those
  names. The party's keys are its leader's.
- Join order: the joiner gets the party's and every other member's data before its `PartyJoinSuccess`, and the
  others get the joiner's before `PartyJoinNotify` (the game reads `headsettype` inside `PartyMemberJoinedCB`).
  The runtime holds data for the party it is joining and on success adds every member it names. Match admission
  re-sends the entrant's data (and the party's, for its leader), off the admission path.
- Runtime: the facade's Update loads changed data with the game's `CJson_LoadFromBuffer` (`0x1405f0bd0`) into
  `+0x1F0` (a member's view of the party) and slot i of the member array (slot 0, the local member's own, is
  never loaded), before the frame's events fire, then fires MemberUpdated / Updated; a slot whose member changed
  is reloaded or cleared (`0x1405ece60`). It shares the leader's party JSON when flags bit 0 is set (then clears
  it) and the local member's after slot 30 handed it out, each serialised by the game's own writer
  (`0x1405f1dc0`). All five functions are prologue-checked at install; with any missing, party data is off and
  the rest of the party works as before.

## Join policy request

Slot 16 `SetJoinPolicy` (from the `R15NetPartySetJoinPolicyNode` handler `0x14018ab90`, and with 0 from
`PartyLeftCB` `0x140189a10`) and slot 21 `JoinPolicy`. Values 0..3 are invite only, friends, friends of
members, everyone (pnsovr mapped them to Oculus 4, 3, 2, 1 in slot 16, `0x180092470`). The runtime sends
`SNSPartySetJoinPolicyRequest` (`kSetJoinPolicyRequest` in `social_party.h`): the standard 0x28 header with
TargetParam = policy (0..3), leader only; the reply is `SNSPartyUpdateSuccess` and members get an
`SNSPartyUpdateNotify`. The scenario `party_lock` asserts the request leaves with the policy.

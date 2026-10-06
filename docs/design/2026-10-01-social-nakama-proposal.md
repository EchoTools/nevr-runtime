# Nakama support for the remaining social features (proposal)

2026-10-01. A proposal, not an implementation: nothing here is built. It answers the "Needs the
owner" list in `docs/design/2026-10-01-social-features-test-plan.md`, under the owner's ruling (via
Spritz): "all those oculus features exist in nakama in one form or another, we just need to support
them, and that can include modifying an evr message", and "stay away from matchmaking, because i've
already got that handled". So #10 (in-game party vs Nakama's matchmaking group) is out, and so is the
part of #19 that carries a leader's lobby for followers (see §4). In scope: a friend's party and
status text (#18/#21), recently met (#22), party data sharing (#19), join policy (#17), the
invitable-users button (#13), and what else the game needs on the way.

Paths: Nakama is `~/src/nakama/server/` (cited as `server/...`), the runtime is this repo. Anything
not read in code is marked **UNVERIFIED**.

## 0. Ground rules every feature below uses

**Who reads these messages.** The game's own social layer (pnsovr) is replaced by the runtime's
facade; friend and party frames are parsed by the bridge (`src/runtime/compat/social_party.h`
`Feed`, `social_roster.h` `ParseStatusNotify`/`ParseListResponse`) and drive the facade's slots. The
bridge also forwards every frame to the game; whether the game does anything with a social symbol it
does not know is **UNVERIFIED** (nothing in the runtime records it; friend and party frames have been
forwarded for weeks without a reported effect).

**Old clients.** The bridge's parsers check minimum lengths only (`len >= 8/16` in `Feed`,
`social_party.h:511`; `len < 17` in `ParseStatusNotify`, `social_roster.h:44`), so bytes appended to
an existing server-to-client message are ignored by old clients. New message symbols reach old
clients' games unparsed (see above). Nakama's decoder requires the frame length to match
(`server/evr/core_packet.go` `ParsePacket`, ~:371); whether a struct's `Stream` tolerates trailing
bytes on a client-to-server message is **UNVERIFIED**, so new client data travels in new message
types, not in widened requests.

**Capability, not build number.** `LoginProfile` (`server/evr/login_request.go:54`) carries the
game's build (`buildversion`, one value for every client), not the runtime's. The bridge writes the
login JSON itself (`src/runtime/compat/ws_bridge.cpp:563-654`), so it can add
`"nevr_social": <level>`; Nakama adds the field to `LoginProfile`, keeps it in the session
parameters, and sends the new messages below only to sessions that declared the level that knows
them. Level 1 = this proposal.

**Adding a message** takes three places in Nakama (`server/evr/core_packet.go:25` `SymbolTypes`,
`server/evr/core_packet_types.go` `NewMessageFromHash` ~:73, `server/evr/types.go` ~:135) plus a
`case *evr.X:` in `server/evr_pipeline.go` (party cases :610-632, friend cases :636-644) for
client-to-server types; the symbol is `ToSymbol(Token())`. The runtime adds the symbol to
`social_party.h` (request constants :33-44, `ReplyTable` :53-72). Before implementing, check each new
token's hash against `server/evr/core_hash_lookup.go` for a collision (the names below are not in it:
checked by name, not by hash).

**Proof.** Each feature has (a) a Nakama unit test of the pure builder or policy function, like the
existing `server/evr_pipeline_party_invite_test.go`; (b) a scenario in the runtime's suite
(`just scenario-all`) that injects the new server message and asserts the game-visible slots, as
`party_invite_join` does for PartyJoinSuccess; and (c) for the server half end to end, the same
scenario against the local Nakama (removed in 45b6465; `git show 941068c4cc6bd5e634052e59f45780763ba36d9f:tools/nakama-local/setup.py`), which a one-client suite
can only do for one side of a two-person feature (the other side is a second session, marked where it
applies).

## 1. A friend's party and status text (#18, #21)

**What the game needs.** Slots 52 FriendStatusString, 54 FriendIsJoinable, 55 FriendPartyId
(`src/runtime/patch/social_facade_object.cpp`, now stubs). pnsovr filled them from each friend's
Oculus presence: party id from the presence deeplink (roster +0x78, callback 0x180085a30; slot 55
0x180085060), status text from the presence string (slot 52 0x1800850e0, pool +0x448); the game cuts
the text at the first `|` (FUN_140da5d80).

**What the game publishes.** `SyncRichPresence` (0x1401b9510) builds
`{game_type, lobby_id, party_id, current_capacity, max_capacity, joinable}` (FUN_140614e00, passed to
the rich-presence object's slot 14 at 0x140614f10) and writes the local member's `status` key as
`"<destination display name> | <second field>"` (format 0x1416e1450) into the member JSON from slot
30. pnsovr sent party_id as the deeplink only when `joinable` (0x180093279). The rich-presence object
(`netGame+0x647c0`, pnsovr's CNSOVRRichPresence, vtable 0x1801fbca8) has no runtime replacement today
(`grep` of `src/runtime` for 0x647c0: nothing), so what the game currently does with it is
**UNVERIFIED**; the second field of the status string (`[game+8]+0x840`) is **UNVERIFIED**.

**Nakama primitive: status presence.** Each session tracks a status presence with
`PresenceMeta.Status`, up to 2048 characters (`server/pipeline_status.go:261-277`), and
`StatusRegistry` pushes changes to followers (`server/status_registry.go:33-44`: `Follow`,
`Unfollow`, `Queue`). The EVR login tracks it with an empty status and follows only itself
(`server/evr_pipeline_login.go:1212`, :1233-1236); friends are polled per refresh
(`sendFriendListResponse`, `server/evr_pipeline_friends.go:496`). A friend's party is
`params.currentSNSPartyID` (`server/evr_session_parameters.go:61-62`) and its handler's `Open` and size
(`server/party_handler.go:39-53`).

**Messages.**
- New client-to-server `SNSPresenceUpdateRequest`: RoutingID u64, LocalUserUUID [16], SessionGUID
  u64 (the standard header, as `SNSFriendListRefreshRequest`), then `json_len u32` and the JSON
  `{game_type, status, lobby_id, party_id, joinable, current_capacity, max_capacity}`. The bridge sends
  it when the game's rich presence changes (it needs a rich-presence facade at `netGame+0x647c0`, or a
  hook on FUN_140614e00 and the `status` write; client work, **UNVERIFIED** which is cleaner).
- Server: `tracker.Update` the session's status presence with that JSON as `Status` (the existing
  stream). The 2048-character cap is enforced only in the realtime `statusUpdate` handler
  (`server/pipeline_status.go:269`), not by `tracker.Update`, so the EVR handler must cap the JSON
  itself. At login `Follow` (`server/status_registry.go:186`) the
  user's friend ids, not only itself. That `Follow` delivers status-text changes, and not only
  presence joins and leaves, is **UNVERIFIED** (`server/status_registry.go` event path not traced).
- New server-to-client `SNSFriendPresenceNotify`: Header u64, FriendID u64, PartyID u64, Joinable u8,
  Status u8 (online/busy/offline, as FriendStatusNotify), Reserved [6], `text_len u16`, text (the
  `status` string). Sent once per friend in the friend-list refresh after the existing
  `SNSFriendStatusNotify` (kept for old clients), and on every followed status change. `PartyID` is 0
  and `Joinable` 0 when the friend is in no party or the party would refuse this viewer (join policy,
  §4); joinability is computed per viewer on the server.
- Why not widen `SNSFriendStatusNotify` (Header, FriendID, StatusCode, Reserved [7],
  `server/evr/sns_friends.go:175`)? The 7 reserved bytes hold the joinable flag but not a party id or
  text; a new message keeps the old one exact.

**Server code.** `server/evr_pipeline_friends.go` (the presence request handler, the notify builder
next to `friendStatusNotifications`, the per-viewer joinable check), `server/evr_pipeline_login.go`
(follow friends at login), `server/evr/sns_friends.go` (the two types).

**Runtime.** Roster gains party id, joinable and status text per friend; slots 52/54/55 return them;
slot 53 FriendIsInvitable can then also exclude a friend already in my party.

**Proof.** Nakama: a test of the notify builder and of the per-viewer joinable rule. Suite: a
`friend_presence` scenario injects `SNSFriendPresenceNotify` for a friend and asserts the slots (52
text, 55 party, 54 joinable) through state, then fires the party-join node for that party (already
proven in `party_join_by_id`). The publish half: a scenario asserts `SNSPresenceUpdateRequest`
leaves with the game's own `game_type` and `status`. Two people: one session sets presence, the other
sees it; needs a second session (local Nakama, two clients), **not** in the one-client suite.

## 2. Recently met (#22)

**What the game needs.** Slots 56-67, getters over Oculus' recently-met list (slot 57 refresh
0x180091070, callback 0x180089ef0): count (58), online count (59), id (61), name and status string
(62/64, pool +0x700), status (63: online if `idx < online`), invitable (65), joinable (66), party id
(67, a u64 per entry; whether it is a party id or an org-scoped id is **UNVERIFIED**, 0x180090b80). The
game refreshes through R15NetRefreshRecentlyMetUsersNode (0x140ddfcc0 -> 0x14019b870 -> slot 57) and
polls slot 56 until it returns 0. No cap was found in pnsovr (it follows Oculus' pages).

**Nakama primitive: storage.** A per-user object, collection `RecentlyMet`, key `list`, read
permission owner only (`server/core_storage.go:583` `StorageWriteObjects`, :427
`StorageReadObjects`), holding up to N entries `{user_id, account_id, display_name, last_met}`.
Today nothing records who played with whom: `MatchHistory` stores the match label, system-owned
(`server/evr_match_label.go:20`, :841-856); the match summary with per-player participation goes only to
MongoDB when configured (`server/evr_runtime_event_match_summary.go:24-60`, :112-117).

**Write.** When a player leaves a match (`MatchLeave`, `server/evr_match.go:792`), add the other players
then present (`MatchLabel.Players`) to their list, newest first, deduplicated, capped (proposal:
N = 50, **UNVERIFIED** against what Oculus returned), skipping blocked users (friend state 3,
`server/core_friend.go:714`). One storage write per leaving player; private and social lobbies included
(owner's call to exclude social lobbies, where you "meet" everyone in the room).

**Messages.**
- New `SNSRecentlyMetRefreshRequest` (the standard 0x20 header) when slot 57 runs.
- New `SNSRecentlyMetListResponse`: Count u32, then per entry: AccountID u64, PartyID u64, Joinable u8,
  Status u8, Reserved [6], `name_len u16` + name, `text_len u16` + status text. Online, party and text
  are resolved at read time from §1's presence (so §2 depends on §1 for everything but names).

**Server code.** `server/evr_match.go` (the write at leave), a small `server/evr_recently_met.go`
(storage model, read, response builder), the handler in `server/evr_pipeline_friends.go`.

**Runtime.** Slots 56-67 over a list fed by the response; slot 56 reports "refreshing" between the
request and the response.

**As built** (nakama `a2162c3bf`, runtime `74a19e3`).
- Who counts as met: at a player's leave, everyone in the match's `participations` (everyone who ever
  joined) whose time there overlapped theirs, so both sides of a meeting record it whoever leaves
  first; moderators (invisible) are never met; private and social lobbies included. Written to
  `RecentlyMet`/`list` (owner read, no client write) off the match loop, retried once on a version
  conflict; logged "Recently met recorded".
- Blocks either way are left out at write and again at read (a block made after the meeting).
- Slot contracts from ReVault (pnsovr 0x180091200..0x180090b80): slot 56 is busy from slot 57 until
  the answer; the facade also ends it after 5 s, because a server without the message never answers
  and the refresh node (0x140ddfcc0) polls slot 56 until 0. Slot 63 is 2 for the online prefix, else
  0. Slot 65 needs our party joinable (slot 22), the person online and not a member. The game keeps
  slot 64's text up to its first `|` (0x140da5d80).

**Proof.** Nakama: tests of the dedupe/cap/blocked rules on the storage model. Suite: a
`recently_met` scenario fires the refresh node (the action exists: `refresh_recently_met`), asserts
the request leaves, injects a response with two entries, and asserts count, ids, names and status
through state and the game's R15NetRecentlyMetUser expression (out-of-range log 0x141cbed70 must not
fire).

## 3. Party and member data (#19)

**What the game needs.** The host writes the party JSON (`social+0x1f0`, via 0x14015fdb0, which also
sets flags bit 0) and every member writes its own member JSON (slot 30, `+0x248 + idx*16`); keys come
from scripts (Set*Data nodes, handlers 0x1401b05a0.., key passed in), so they cannot be listed from
the exe. The runtime itself must also fill keys the game reads in C++: `headsettype` per member
(read at 0x14018a090 and in PartyMemberJoinedCB; today every remote member shows "(Unknown)" in
"'%s' joined party (%s)"). pnsovr shared both from its Update (0x1800ac240: slot 7 for the party JSON
when bit 0 is set, slot 6 per dirty member), through the Oculus room data store with a `seqid`
(0x180082f30 rejects older ones), and delivered remote member JSON by packet (0x180090650 ->
0x180082cd0, firing MemberJoined first, MemberUpdated after).

**Out of scope here (matchmaking).** pnsovr also stamped `lobbyid`, `matchtype`, `team`,
`lobbytype`, `offline` (0x1800ac430) into the party JSON, and a non-host member reads the leader's
`lobbyid`/`matchtype` from it (0x14017c6a0, 0x140189d70) to follow. That is the party-follow path;
the data channel below would carry those keys like any other, but whether and how a follower acts on
them is the owner's matchmaking work.

**Nakama primitive: parties.** `PartyHandler.DataSend` relays op-coded data to the other members
(`server/party_handler.go:703`) but keeps nothing, so a member who joins later never sees it. Today
`snsPartyUpdateRequest`/`snsPartyUpdateMemberRequest` only broadcast an empty notify
(`server/evr_pipeline_party.go:717-748`). Proposal: the SNS party keeps the latest party JSON and one
JSON per member, each with a `seqid`, on the party's state in `server/evr_pipeline_party.go` (next to
`snsPartyInvites`) or on `PartyHandler` (`server/party_handler.go`, guarded by its lock).

**Messages.** (As built: nakama `251535cee`, `f1658d1dc`, `d39d09914`; the runtime commit that follows this doc
change. Two departures from the first draft, below.)
- `SNSPartyDataUpdateRequest` (0x3448ca6e8d9dd0ce): the standard 0x28 header with TargetParam = scope
  (0 party, 1 member), then `seqid u32`, `json_len u32`, a JSON object of at most 4 KiB. Party scope
  from the leader only (else `SNSPartyUpdateFailure` code 2); member scope from the sender for itself.
  The reply is `SNSPartyUpdateSuccess` / `SNSPartyUpdateMemberSuccess`. A write whose seqid is not newer
  than the stored one from the same session is acknowledged and not kept.
- Server to client: a new `SNSPartyDataNotify` (0x832143ccbf160955): PartyID(8), MemberID(8, 0 = the
  party's data), seqid(4), json_len(4), JSON. *Departure:* not bytes appended to
  `SNSPartyUpdateNotify`/`SNSPartyUpdateMemberNotify`; the relay picks recipients by capability level
  anyway, so a message only level-1 clients get needs no trailing-bytes question answered.
- The server fills `lobbyid` (upper-case match GUID, or the nil GUID), `matchtype` (the mode symbol, or
  -1), `team` (or 65535), `lobbytype` (or 2) and `offline` (no status presence) for the writer, plus
  `headsettype` (1 Rift, 2 Rift S, 3 Quest, 4 Quest on PC, else 0, from the login profile) for a
  member, over whatever the client sent under those names (owner, 2026-10-01: filled by the server,
  nothing blank). The party's keys are its leader's.
- Join: *departure* from "after `PartyJoinSuccess`". The game reads `headsettype` inside
  PartyMemberJoinedCB, so the data must arrive first: the joiner gets the party's and every other
  member's data before its `PartyJoinSuccess`, and the others get the joiner's before
  `PartyJoinNotify` (nakama `snsPartyDataJoining`). The runtime holds data for the party it is joining
  and on success adds every member it names, as pnsovr added a member when its data packet arrived
  (0x180090650 -> 0x180082cd0); this also gives a joiner the members besides the leader, which
  `PartyJoinSuccess` alone never named. Match admission re-sends the entrant's data (and the party's,
  for its leader), off the admission path.

**Runtime.** The facade's Update loads changed data with the game's `CJson_LoadFromBuffer`
(0x1405f0bd0, which releases the old document once the new one parses) into `+0x1F0` (a member's view
of the party) and slot i of the member array (slot 0, the local member's own, is never loaded), before
the frame's events fire, then fires MemberUpdated / Updated; a slot whose member changed is reloaded or
cleared (0x1405ece60). It shares the leader's party JSON when flags bit 0 is set (then clears it) and
the local member's after slot 30 handed it out, each serialised by the game's own writer (0x1405f1dc0,
as `Send` 0x14060e380 does), and both once on entering a party. All five functions are
prologue-checked at install; with any missing, party data is off and the rest of the party works as
before.

**Proof.** Nakama: tests that the store keeps the newest seqid, rejects a non-leader party write, and
builds the join snapshot. Suite: `party_data` already proves the game's writes; it gains the send
(`SNSPartyDataUpdateRequest` leaves with the written key after the dirty bit) and an injected
`SNSPartyUpdateMemberNotify` with `{"headsettype":1}` for a member, after which the game's own
"'<name>' joined party (<headset>)" names a headset instead of "Unknown".

## 4. Join policy (#17)

**What the game needs.** Slot 16 SetJoinPolicy (from the R15NetPartySetJoinPolicyNode handler
0x14018ab90, and with 0 from PartyLeftCB 0x140189a10) and slot 21 JoinPolicy (read by the
R15NetPartyJoinPolicyExpression, 0x140189650 -> 0x140d9cf20). Values 0..3 = invite only, friends,
friends of members, everyone (pnsovr mapped them to Oculus 4, 3, 2, 1 in slot 16, 0x180092470). No
C++ use of the policy beyond that was found; scripts may hide buttons on it (**UNVERIFIED**, scripts
not searched). The facade stores it today (`party_lock` passes).

**Nakama primitive: parties and friends.** `PartyHandler.Open` with join requests for a closed party
(`server/party_handler.go:150-186`), already driven by Lock/Unlock
(`server/evr_pipeline_party.go:407-453`). Friends: `GetFriendIDs` (`server/core_friend.go:46`) returns
every `user_edge` state (invites sent and received and blocked too) and is marked "only used ... for the
console" (:45), so the rule below needs a query for state 0 (mutual friends) and a mapping from the
EVR account to the Nakama user id. `ListFriendsOfFriends` (:328) is paged and excludes direct friends,
so it cannot back the rule: "friends of members" is "a mutual friend of any current member", one
state-0 query per member (at most 4). SNS parties are always created open, size 4
(`server/evr_pipeline_party.go:249`).

**Messages.** New `SNSPartySetJoinPolicyRequest`: the standard 0x28 header with TargetParam = policy
(0..3), leader only; reply `SNSPartyUpdateSuccess` (exists) and an `SNSPartyUpdateNotify` to members
(exists). The policy is party state next to the data of §3.

**Enforcement** in `snsPartyJoinRequest` (`server/evr_pipeline_party.go:271`) and the invite accept
(`snsPartyRespondToInviteRequest`): a pending invite always admits (invited is the point of every
policy); otherwise invite only refuses, friends needs the joiner to be a state-0 friend of the leader,
friends of members a state-0 friend of any member, everyone admits. The same rule feeds §1's
per-viewer `Joinable`.

**New behaviour, not a mapping.** Today a closed (`Open = false`, locked) party does not refuse: the
join is queued for the leader's approval and the joiner gets no reply
(`server/evr_pipeline_party.go:304-307`), and `partyJoinFailureCode` returns only 5, 1 or 2 (nakama
f808932a8). Refusing with `SNSPartyJoinFailure` code 3 (the game's "no permission",
`PartyJoinFailedCB` 0x140189590) for a policy refusal, and code 4 ("locked") for a locked party, are
both new; whether a locked party should refuse or keep queueing for approval is the owner's call.

**Matchmaking-adjacent, for the owner.** The policy check sits in `snsPartyJoinRequest` next to
`createReservationForNewPartyMember` (`server/evr_pipeline_party.go:334`), and a join that goes through
calls `PartyHandler.JoinRequest`, which stops the party's matchmaking (`matchmaker.RemovePartyAll`,
`server/party_handler.go:154`). The proposal only refuses joins earlier and does not change either
call, but it is in the same function as matchmaking work.

**Proof.** Nakama: a table test of the admit rule (policy x invited x friend x friend-of-member).
Suite: `party_lock` gains the request (`SNSPartySetJoinPolicyRequest` leaves with the policy when the
node runs), and `party_join_errors` already proves code 3 shows "no permission". The refusal of a real
second account needs two sessions (local Nakama), **not** in the one-client suite.

## 5. The invitable-users button (#13, slot 38)

pnsovr opened the Oculus "invite users" overlay (ovr_Room_LaunchInvitableUserFlow, 0x18008fe40);
there is no friends or invite panel in the exe for a PC replacement to open (searched: no caller of
slot 38 but 0x140187170 from the InviteUsers node, mode 0; no "invitable"/overlay UI strings). So
**no Nakama change**: the PC path to invite is the friends list row's invite button (`invite`
scenario), and with §1 the row can show who is invitable (online, not in my party, party joinable).
What slot 38 should do on PC (nothing, or a script-side panel) stays the owner's UI call; the server
already has everything such a panel would list (§1).

## 6. Also found

- `SNSFriendStatusNotify`'s busy state (1) is never sent (`friendStatusCode` returns 0 or 2,
  `server/evr_pipeline_friends.go:453`); with §1 a friend in a match could be "busy" from the presence
  `game_type`. Small, optional.
- The friend list is polled, not pushed (`sendFriendListResponse`); §1's follow makes friend status
  changes live for free.

## 7. Order and size

1. §0 capability field (login JSON, `LoginProfile`, session params): small, unlocks everything.
2. §4 join policy: one request, one rule, no client UI work; proves the gating end to end.
3. §1 presence: the largest client piece (rich-presence facade) and the base for §2.
4. §3 party data: server store and snapshot, client sync and CJson load.
5. §2 recently met: storage write at match leave, then the list.

Each step lands with its Nakama tests and its scenario in `just scenario-all`; none touches
matchmaking.

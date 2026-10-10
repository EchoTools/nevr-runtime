# Nakama support for the remaining social features (proposal)

2026-10-01. A proposal, partly built. The built parts (the login capability field, the receive half of the
friend presence, recently met, party and member data, and the join-policy request) are documented as they
are in `docs/reference/social-nakama-protocol.md`. What is still proposed and not built, and so is the
approved design this document holds: the presence publish (`SNSPresenceUpdateRequest`, which has no runtime
sender), the join-policy enforcement rule, §5 and §6, and the order in §7. It answers the "Needs the owner" list
in `docs/design/2026-10-01-social-features-test-plan.md`, under the owner's ruling (via Spritz): "all those
oculus features exist in nakama in one form or another, we just need to support them, and that can include
modifying an evr message", and "stay away from matchmaking, because i've already got that handled". So #10
(in-game party vs Nakama's matchmaking group) is out, and so is the part of #19 that carries a leader's lobby
for followers.

Paths: Nakama is `~/src/nakama/server/` (cited as `server/...`), the runtime is this repo. Anything not read in
code is marked **UNVERIFIED**.

## 1. The presence publish (#18, #21)

The receive half is built (`SNSFriendPresenceNotify`, see the reference). Not built: the runtime does not send
the local presence.

**What the game publishes.** `SyncRichPresence` (0x1401b9510) builds
`{game_type, lobby_id, party_id, current_capacity, max_capacity, joinable}` (FUN_140614e00, passed to
the rich-presence object's slot 14 at 0x140614f10) and writes the local member's `status` key as
`"<destination display name> | <second field>"` (format 0x1416e1450) into the member JSON from slot
30. pnsovr sent party_id as the deeplink only when `joinable` (0x180093279). The rich-presence object
(`netGame+0x647c0`, pnsovr's CNSOVRRichPresence, vtable 0x1801fbca8) has no runtime replacement today
(`grep` of `src/runtime` for 0x647c0: nothing), so what the game currently does with it is
**UNVERIFIED**; the second field of the status string (`[game+8]+0x840`) is **UNVERIFIED**.

**Nakama primitive: status presence.** Each session tracks a status presence with
`PresenceMeta.Status`, up to 2048 characters (`server/pipeline_status.go`), and
`StatusRegistry` pushes changes to followers (`server/status_registry.go`: `Follow`,
`Unfollow`, `Queue`). The EVR login tracks it with an empty status and follows only itself
(`server/evr_pipeline_login.go`); friends are polled per refresh
(`sendFriendListResponse`, `server/evr_pipeline_friends.go`). A friend's party is
`params.currentSNSPartyID` (`server/evr_session_parameters.go`) and its handler's `Open` and size
(`server/party_handler.go`).

**Messages.**
- New client-to-server `SNSPresenceUpdateRequest`: RoutingID u64, LocalUserUUID [16], SessionGUID
  u64 (the standard header, as `SNSFriendListRefreshRequest`), then `json_len u32` and the JSON
  `{game_type, status, lobby_id, party_id, joinable, current_capacity, max_capacity}`. The bridge sends
  it when the game's rich presence changes (it needs a rich-presence facade at `netGame+0x647c0`, or a
  hook on FUN_140614e00 and the `status` write; client work, **UNVERIFIED** which is cleaner).
- Server: `tracker.Update` the session's status presence with that JSON as `Status` (the existing
  stream). The 2048-character cap is enforced only in the realtime `statusUpdate` handler
  (`server/pipeline_status.go`), not by `tracker.Update`, so the EVR handler must cap the JSON
  itself. At login `Follow` (`server/status_registry.go`) the
  user's friend ids, not only itself. That `Follow` delivers status-text changes, and not only
  presence joins and leaves, is **UNVERIFIED** (`server/status_registry.go` event path not traced).
- A server-to-client `SNSFriendPresenceNotify` is already sent for each followed change (see the reference);
  the runtime half of this section is the publish only.

**Server code.** `server/evr_pipeline_friends.go` (the presence request handler), `server/evr_pipeline_login.go`
(follow friends at login), `server/evr/sns_friends.go` (the request type). Runtime: a rich-presence facade at
`netGame+0x647c0`, or a hook on `FUN_140614e00` and the `status` write.

**Proof.** The publish half: a scenario asserts `SNSPresenceUpdateRequest` leaves with the game's own
`game_type` and `status`. Two people: one session sets presence, the other sees it; needs a second session
(local Nakama, two clients), **not** in the one-client suite.

## 2. Join policy enforcement (#17)

The join-policy request is built (see the reference). Not built: enforcing the policy in Nakama.

**Nakama primitive: parties and friends.** `PartyHandler.Open` with join requests for a closed party
(`server/party_handler.go`), already driven by Lock/Unlock (`server/evr_pipeline_party.go`). Friends:
`GetFriendIDs` (`server/core_friend.go`) returns every `user_edge` state (invites sent and received and
blocked too) and is marked "only used ... for the console" in the same file, so the rule below needs a query
for state 0 (mutual friends) and a mapping from the EVR account to the Nakama user id. `ListFriendsOfFriends`
in `server/core_friend.go` is paged and excludes direct friends, so it cannot back the rule: "friends of
members" is "a mutual friend of any current member", one state-0 query per member (at most 4). SNS parties
are always created open, size 4 (`server/evr_pipeline_party.go`).

**Enforcement** in `snsPartyJoinRequest` (`server/evr_pipeline_party.go`) and the invite accept
(`snsPartyRespondToInviteRequest`): a pending invite always admits (invited is the point of every
policy); otherwise invite only refuses, friends needs the joiner to be a state-0 friend of the leader,
friends of members a state-0 friend of any member, everyone admits. The same rule feeds §1's
per-viewer `Joinable`.

**New behaviour, not a mapping.** Today a closed (`Open = false`, locked) party does not refuse: the
join is queued for the leader's approval and the joiner gets no reply
(`server/evr_pipeline_party.go`), and `partyJoinFailureCode` returns only 5, 1 or 2 (nakama
f808932a8). Refusing with `SNSPartyJoinFailure` code 3 (the game's "no permission",
`PartyJoinFailedCB` 0x140189590) for a policy refusal, and code 4 ("locked") for a locked party, are
both new; whether a locked party should refuse or keep queueing for approval is the owner's call.

**Matchmaking-adjacent, for the owner.** The policy check sits in `snsPartyJoinRequest` next to
`createReservationForNewPartyMember` (`server/evr_pipeline_party.go`), and a join that goes through
calls `PartyHandler.JoinRequest`, which stops the party's matchmaking (`matchmaker.RemovePartyAll`,
`server/party_handler.go`). The proposal only refuses joins earlier and does not change either
call, but it is in the same function as matchmaking work.

**Proof.** Nakama: a table test of the admit rule (policy x invited x friend x friend-of-member).
`party_join_errors` already proves code 3 shows "no permission". The refusal of a real second account needs
two sessions (local Nakama), **not** in the one-client suite.


## 3. The invitable-users button (#13, slot 38)

pnsovr opened the Oculus "invite users" overlay (ovr_Room_LaunchInvitableUserFlow, 0x18008fe40);
there is no friends or invite panel in the exe for a PC replacement to open (searched: no caller of
slot 38 but 0x140187170 from the InviteUsers node, mode 0; no "invitable"/overlay UI strings). So
**no Nakama change**: the PC path to invite is the friends list row's invite button (`invite`
scenario), and with §1 the row can show who is invitable (online, not in my party, party joinable).
What slot 38 should do on PC (nothing, or a script-side panel) stays the owner's UI call; the server
already has everything such a panel would list (§1).

## 4. Also found

- `SNSFriendStatusNotify`'s busy state (1) is never sent (`friendStatusCode` returns 0 or 2,
  `server/evr_pipeline_friends.go`); with §1 a friend in a match could be "busy" from the presence
  `game_type`. Small, optional.
- The friend list is polled, not pushed (`sendFriendListResponse`); §1's follow makes friend status
  changes live for free.

## 5. Order and size

1. §2 join policy enforcement: one rule, no client UI work; proves the gating end to end.
2. §1 presence publish: the largest client piece (a rich-presence facade).

Each step lands with its Nakama tests and its scenario in `just scenario-all`; none touches matchmaking.

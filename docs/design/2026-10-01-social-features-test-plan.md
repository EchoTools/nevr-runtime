# Social features: prioritized list and how each is tested

2026-10-01. Companion to `docs/design/2026-10-01-social-scenario-harness.md` (the harness this
list runs on). Owner's order: "create a prioritized list of all social features and how it's
going to test them." Owner's ruling on the first version: "accept, invite etc. are all part of
it. there also is and add i think.. where you can add somebody as a friend, in game. i want all
the features" / "they just need to be prioritize."

## How the list was built (complete by construction)

Every feature below comes from one or more of these sources, named in the **Source** column:

- **Node**: the game's own social script nodes, the things the menus actually run. Names are
  from echovr.exe's strings (`R15Net*Node`); each node's run function was resolved from the
  registration code that pairs a node name with its factory and run function (three consecutive
  rip-relative LEAs, e.g. `R15NetAddFriendNode` at 0x140f4eb93 -> factory 0x140e447a0 -> run
  0x140dd90f0). The catalog is the next section. This was the source the first version missed.
- **Slot N**: the game's Social interface, as implemented by the facade
  (`src/runtime/patch/social_facade_object.cpp`, the `kVtable` table: 75 slots, each traced).
- **Event**: a `delegate_on*` session event name from the game's symbol table
  (`src/runtime/log/symcache_data.cpp`; 104 delegate names, 29 of them social).
- **Msg**: an SNS wire message the game's protocol defines, and what Nakama does with it
  (EchoTools nakama `server/evr_pipeline_friends.go`, `server/evr/sns_friends.go`).
- **Caller**: one of the 19 echovr.exe callers of `CNSProvider::UserProviderID` (0x1406186d0),
  mapped in the last section.

Also checked: echovr.exe looks up exactly three provider exports by name, `Social`, `Users` and
`RichPresence` (the only exact-match strings in the binary). pnsrad exports `Friends`, `Party` and
`Activities` (its own CNSRADFriends, with Invite/Accept/Reject/Remove/Withdrawn callbacks), but
the game never asks for them, so that code is unused. pnsovr's only friend API is
`ovr_User_GetLoggedInUserFriends`; on Oculus, friend requests were handled in the platform
overlay that `OpenFriendRequestUI` opened.

Status vocabulary: **works** = seen working in a run log, with the run named; **broken** =
seen failing; **stub** = the facade slot returns a constant or does nothing; **unknown** = never
exercised; **absent** = the game has no node, slot or message for it (evidence given).

A scenario enters the game only through the game's own entry point: the node's run function,
replicated or fired the way `fire friend_invite` replicates `R15NetPartySendInviteNode`. The
handler each node posts is listed in the catalog; which facade slot that handler reaches is
**to read** where not yet disassembled.

**Destructive** = changes a real account's friendships or a real party (remove, kick, leave or
promote in a real party). These stay **BLOCKED** until the owner names a test friend.

## Social script-node catalog (the in-game entry points)

Run functions from the registration table; "posts" = the handler each run function hands to
the NetGame deferred queue (0x140f4b690) or calls, from a scan of its instructions.

| Node | Run | Posts / calls | Reaches |
|---|---|---|---|
| R15NetAddFriendNode | 0x140dd90f0 | posts 0x1401870f0 | three gates, then social slot 37 OpenFriendRequestUI(0, account) (raw disassembly of the caller pattern; agent read of 0x1401870f0) |
| R15NetPartySendInviteNode | 0x140dddf60 | posts 0x14018aa90 | three gates, then 0x140614320 -> slot 8 SendInvite (raw disassembly; scenario 143852) |
| R15NetInviteUsersNode | 0x140ddc700 | data: user string [0..0x28), u32 mode at +0x40. mode 0 -> 0x140187170; 1 -> 0x140187330, with a user 0x140187230; 2 -> 0x1401874f0, with a user 0x1401873f0 | slots 38, 40/39, 43/42; all gated on npe\|firstmatch\|completed and online, the user ones also on the provider (raw disassembly; party_ui_buttons) |
| R15NetPartyRespondToInviteNode | 0x140dddd30 | 0x140188bf0, 0x140188f40 via 0x140198650 (index) | accept: slot 70 then 73; dismiss: slot 74 (party_invite_accept, party_invite_dismiss) |
| R15NetPartyJoinNode | 0x140ddd3a0 | data: u64 party id; calls 0x140189570(netGame, id) | 0x14060a310 -> slot 2 JoinInternal; a zero id is ignored (party_join_by_id) |
| R15NetPartyLeaveNode | 0x140ddd790 | posts 0x1401899f0 | to read |
| R15NetPartyKickNode | 0x140ddd610 | posts 0x140189910 | to read |
| R15NetPartyPassOwnershipNode | 0x140ddda20 | posts 0x14018aa00 | to read |
| R15NetPartyLockNode | 0x140ddd870 | data: u8 lock, u8 mask; calls 0x140189f20(netGame, lock, mask) | no slot: flips the mask in byte netGame+0x647ea and the host's joinable bit (+0x27C bit 1) on the social object; pnsovr's Update then locks the room (slot 5) (party_lock) |
| R15NetPartySetJoinPolicyNode | 0x140dde110 | data: u32 policy; calls 0x14018ab90(netGame, policy) | slot 16 SetJoinPolicy, then SyncRichPresence (party_lock) |
| R15NetPartySetUnjoinableLobbyNode | 0x140dde2c0 | posts 0x14018abd0 (no argument) | not host: slots 24, 25 and records the party in netGame+0x647d0.. (not yet scenario-tested) |
| R15NetPartyRefreshInvitesNode | 0x140dddb90 | posts 0x14018aa50 | to read (slot 69 RefreshInvites expected) |
| R15NetRefreshFriendsNode | 0x140ddf9a0 | posts 0x14019ae10 | expected slot 45 RefreshFriends (slot 45 is seen called in runs 143852 and 09-30; the handler itself is to read) |
| R15NetRefreshRecentlyMetUsersNode | 0x140ddfcc0 | posts 0x14019b870 (no argument) | slot 57; polls slot 56 (see Needs the owner, #22) |
| R15NetVoipMuteUserNode / MuteLobbyMemberNode / MuteSelfNode | 0x140de6110 / 0x140de5eb0 / 0x140de5fc0 | self: posts 0x1401b24a0 via 0x140198650 (flag); user: 0x140f47cd0; member: 0x140f47a30 | self: bit 38 of [netGame+0x2da0], onvoipmuted/onvoipunmuted (voip_mute); user mute keeps a list at netGame+0x64780 and the profile key mute\|users (not yet tested) |
| R15NetSetParty{Flag,Int,Real,String,Symbol}Node, R15NetSetPartyMember{Flag,Int,Real,String,Symbol}Node | 0x140de2640 ... 0x140de34a0 | data: key [0..0x40) then value; handlers 0x1401b05a0.. (party), 0x1401b0660.. (member), called (netGame, key, value) | party: host only, 0x14015fdb0 marks +0x27C bit 0 and returns the party JSON; member: slot 30 MemberDataWritable (party_data) |
| R15NetSocialGroupsSetActiveNode, R15NetOpenSocialGroupLinkNode, R15NetSocialModMsgShownNode | 0x140d8eba0, 0x140d8e430, 0x140de4a90 | set active: 0x1401adf90(groups = [netGame+0x2a00], index) | a new index raises onsocialgroupschangedactive and writes the profile key social\|group (0x1401b18a0) (social_groups) |
| R15NetEnableSocialFeatureNode | 0x140ddb650 | data: u32 enable, u32 feature 0..4 -> mask 1,2,4,8,0xff; calls 0x140cd3850(mask, enable) | no slot: the u16 at 0x142025bf4, read by gameplay (UpdatePersonalBubble tests mask 2). The earlier "posts 0x14016afc0" was wrong: that is R15NetFindMatchNode (0x140ddb740) (social_features) |
| R15NetRequestProfileNode | 0x140de01f0 | posts 0x1401a1930 with the 16-byte user id | profile manager 0x140612820 -> SNSOtherUserProfileRequest; reply -> "[NETGAME] Profile received for '%s'" (0x14019f510) (request_profile) |
| R15NetAddDebugPartyMemberNode | 0x140d8d000 | | debug only; not a player feature |

Friend-management nodes that do **not** exist: no accept-friend, decline-friend, remove-friend,
block or report node is registered (full list of `R15Net*` social names from the binary's strings:
the catalog above plus expressions such as `R15NetIsFriendExpression`,
`R15NetIsPartyMemberExpression`, `R15NetIsRecentlyMetUserExpression`).

## Priority list

Ordered by what players hit first and most often. Every feature the game exposes is on it; the
owner wants all of them, so the order is build order, not a cut line.

| # | Feature | Source | Why this rank | Status (evidence) | Scenario: setup / do / assert | Risk |
|---|---|---|---|---|---|---|
| 1 | Friends list: names, online/offline, order | Node RefreshFriends; Slots 45-51; Msg FriendListResponse, FriendStatusNotify | Every session opens the friends tab | works (2026-09-30 owner run: names and presence shown; scenario 143852 roster state) | setup: inject FriendListResponse + FriendStatusNotify for 3 ids (2 online). do: fire RefreshFriends (run 0x140ddf9a0). assert: FriendListRefreshRequest sent; slots FriendCount=3, OnlineFriendCount=2, online first, FriendName per index | name lookups go to production Nakama for real ids |
| 2 | Party exists after login (auto create) | Slot 13 Update bit0; Event onpartycreated; Msg PartyCreateRequest/Success | Prerequisite for every party feature | works (scenario 143852 step 3: party id 23, joinable) | already step 3 of the invite scenario | none |
| 3 | Send a party invite to a friend | Node PartySendInvite; Slot 8; Caller 0x14018aa90; Msg PartyInviteRequest | The reason parties exist | works in scenario after 7160561 (143852: SendInvite, PartyInviteRequest target=F); a real click UNVERIFIED (see "Calibration") | the invite scenario (`tools/scenario/scenarios/invite.yaml`); its diagnose list should also name the events onpartyinviteerror{offline,uninvitable,unjoinable,unknown,firstmatchnotcompleted} | the request reaches production; synthetic target |
| 4 | Receive a party invite (it appears in the invite list) | Node PartyRefreshInvites; Slots 70-72; Event onpartyinvitereceived; Msg PartyInviteNotify | The other half of #3 | **works in scenario** (run 20261001T190941: injected PartyInviteNotify fired the InviteReceived callback (index 14), the game logged "Party invite received from F", InviteCount=1, InviteSender(0)=F) | setup: roster with F online. inject: PartyInviteNotify{party P, inviter F}. assert: InviteCount=1, InviteSender(0)=F, event onpartyinvitereceived | none (inbound only) |
| 5 | Accept a party invite (join the party) | Node PartyRespondToInvite (run 0x140dddd30); Slots 73, 2; Event onpartyjoined, onpartymemberjoined; Msg PartyJoinRequest/Success/Failure | Completes the invite loop | **works in scenario**: the whole accept (9bfed8c) into the inviter's party, Joined and MemberJoined callbacks and the game's own "joined party" line (`party_invite_join`, suites 20261001T220327-suite and 20261001T222547-suite); the failure path `party_invite_accept`. Nakama sent nothing for an accept that matched no invite, leaving the client joining forever; fixed in nakama a8eb2de79 (local, not deployed) | after #4: fire PartyRespondToInvite(accept). assert: PartyJoinRequest carries P; inject PartyJoinSuccess [F, self]; assert events onpartyjoined, onpartymemberjoined, MemberCount=2, MemberName(1)=F's name | joining moves the local user's real party; not destructive to anyone else |
| 6 | Add a friend, in game | Node AddFriend (run 0x140dd90f0 -> 0x1401870f0); Slot 37 OpenFriendRequestUI; Msg FriendInviteRequest/Success/Failure | Owner named it; the way friends are made inside the game | **works in scenario** since 0acbeb9: slot 37 sends SNSFriendInviteRequest (run 20261001T160330: slot 37 reached, FriendInviteRequest target=F, Nakama answered FriendInviteFailure for the synthetic F); a real click UNVERIFIED | implement slot 37 as SNSFriendInviteRequest(target account) (the message Nakama handles at evr_pipeline_friends.go:115). Scenario: fire AddFriend for synthetic F; assert slot 37 reached and FriendInviteRequest left with target F; inject FriendInviteSuccess; assert the outcome is shown (event or roster) | sends a real friend request if F is real; synthetic F |
| 7 | Accept a friend request, in game | Node AddFriend on the requester; Msg FriendAcceptSuccess, FriendAcceptNotify | Half of #6 | absent as its own action: no accept-friend node. Nakama turns an add to someone who already requested you into an accept (evr_pipeline_friends.go: "Mutual add" -> SNSFriendAcceptSuccess, and SNSFriendAcceptNotify to the other side), so #6 is accept too | as #6 with a pending request from F: assert FriendInviteRequest leaves and the server answers FriendAcceptSuccess. Not yet run: it needs a real pending request from another account, which a synthetic F cannot make; the client half (FriendAcceptSuccess triggers the roster refresh) passes in friend_changes | as #6 |
| 8 | Friend changes appear without a restart (added, accepted, removed, rejected, withdrawn) | Msg FriendInviteNotify, FriendAcceptNotify, FriendAcceptSuccess, FriendRemoveNotify, FriendRejectNotify, FriendWithdrawnNotify | Feedback for #6/#7, and the 09-30 owner report (a friend added on the website doesn't show) | **works in scenario** since 0acbeb9: every friend-change message triggers a friend-list refresh and the server's list rebuilds the roster (run 20261001T160446: FriendAcceptNotify and FriendRemoveNotify each sent FriendListRefreshRequest, Nakama answered FriendListResponse) | `tools/scenario/scenarios/friend_changes.yaml` | none (inbound only) |
| 9 | Party join errors are shown, not swallowed | Events onpartyjoinerror{full,locked,nopermission,notfound,version,unknown}; Msg PartyJoinFailure | A failed join that says nothing is the 09-30 bug class again | **works in scenario**, every code (run 20261001T193721: notfound, locked, nopermission, full, version and unknown events each raised, then a new party). The game's codes (PartyJoinFailedCB 0x140189590): 1 not found, 3 no permission, 4 locked, 5 full, 6 version, else unknown; the bridge passes them through since 0496f65. Nakama sent every refusal as 2, so "full" could never show; nakama f808932a8 sends 5 for a full party and 1 for a missing or closed one (local only) | `tools/scenario/scenarios/party_join_errors.yaml` | none |
| 10 | Party follows the leader into a match | Slots 31-34 (EnterLobby, ExitLobby and forwarders); Node PartySetUnjoinableLobby; Event onlobbyjoinerrorpartychange, onnetgamemenupartyjoined; Callers 0x1401cbd00, 0x1401889d0 | The point of a party is playing together | **needs the owner** (see "Needs the owner"): Nakama's party follow uses a lobby group from a user setting, not the in-game party, so the game's party never matchmakes together; and a test enters real matchmaking | setup: a party of [self, F] by injection (#5). do: start matchmaking (entry to measure: the matchmaking node). assert: EnterLobby slot calls, and that the lobby request lists both members' ids with code 4 | touches real matchmaking on production |
| 11 | Party member joins or leaves (callbacks, UI roster) | Events onpartymemberjoined, onpartymemberleft, onpartymemberupdated; Callers PartyMemberJoinedCB, PartyMemberLeftCB; Msg PartyJoinNotify, PartyLeaveNotify | Every party changes membership | **works in scenario** (run 20261001T193907: MemberJoined/MemberLeft callbacks, the game's "'<name>' joined party" / "left party" lines, member count 1 -> 2 -> 1). PartyMemberJoinedCB (0x14018a3c0) raises a component event, not a session event. Members and invite senders showed account ids until e62867d (profile lookup, as for friends) | `tools/scenario/scenarios/party_members.yaml` | the matchmaking-cancel branch in both callbacks fires if a search is running (game behaviour) |
| 12 | Dismiss (decline) a party invite | Node PartyRespondToInvite(dismiss); Slot 74; Msg PartyInviteResponse (param 0) | Common, cheap | **works in scenario** (run 20261001T191139: slot 74, PartyInviteResponse(param 0), InviteCount=0, the party untouched) | after #4: fire PartyRespondToInvite(dismiss). assert: PartyInviteResponse(param 0) to F, InviteCount=0 | sends a decline to F's account; synthetic F |
| 13 | Invite users / party UI buttons | Node InviteUsers (run 0x140ddc700); Slots 39-43 | Buttons in the menus | **works in scenario** (routing): every mode of the node reaches its slot, 38, 40/39, 43/42 (`party_ui_buttons`, suites 20261001T220327-suite and 20261001T222547-suite); 39-43 do nothing, as pnsovr's bare `ret`. The slot labels 39/40 and 42/43 were swapped (fixed d2283ee). What slot 38 should open on PC: see "Needs the owner" | fire InviteUsers; assert which slots are reached; then decide what each should do (pnsovr opened the Oculus overlay here: owner's design call) | |
| 14 | Leave the party | Node PartyLeave (run 0x140ddd790 -> 0x1401899f0); Slot 17; Event onpartyleft; Msg PartyLeaveRequest/Success | Every party ends | unknown | setup: party [self, F] by injection. fire PartyLeave. assert: PartyLeaveRequest, event onpartyleft, MemberCount=1 | **Destructive** on a real party. Allowed only on an injected party; BLOCKED for real parties |
| 15 | Kick a member | Node PartyKick (run 0x140ddd610 -> 0x140189910); Slot 19; Event onpartykicked; Msg PartyKickRequest/Notify | Party leader tool | unknown | setup: party [self, F], self host. fire PartyKick(F). assert: PartyKickRequest targets F's member UUID | **Destructive**: BLOCKED until a test friend is named |
| 16 | Promote (pass ownership) | Node PartyPassOwnership (run 0x140ddda20 -> 0x14018aa00); Slot 18; Event onpartyhostchanged; Msg PartyPassRequest/Notify | Party leader tool | unknown | as #15; assert PartyPassRequest, then inject PartyPassNotify and assert IsHost=0 | **Destructive** on a real party: BLOCKED |
| 17 | Lock/unlock the party, join policy | Nodes PartyLock, PartySetJoinPolicy; Slots 16, 21; Event onpartyjoinabilitychanged; Msg PartyLock/Unlock* | Less used | **works in scenario** (lock): the lock node makes the party unjoinable at once, the server locks it (PartyLockRequest, LockSuccess) and unlock reverses it (`party_lock`, suites 20261001T220327-suite and 20261001T222547-suite); slots 4/5 and pnsovr's host sync were missing, so a lock never reached the server (fixed d2283ee). Join policy: slot 16 stores it (same run); a server-side policy needs the owner | fire PartyLock; assert Lock request, Joinable flips, event onpartyjoinabilitychanged | changes a real party's policy; reversible |
| 18 | Join a friend's party from the friends list | Node PartyJoin (run 0x140ddd3a0); Slots 54 FriendIsJoinable, 55 FriendPartyId | Shortcut players expect | **works in scenario** (join by id): the party-join node reaches slot 2, the request goes out and the real server's "not found" comes back (`party_join_by_id`, suites 20261001T220327-suite and 20261001T222547-suite). A friend's party id (slots 54/55): see "Needs the owner" | first implement from presence data; then fire PartyJoin for F's party; assert PartyJoinRequest | needs a server source for a friend's party id |
| 19 | Party and member shared data | Nodes SetParty*/SetPartyMember*; Slots 6 PushMemberData, 7 ShareData; Msg PartyUpdate*, PartyUpdateMember* | Used by menus for party state | **works in scenario** (the game's writes): member data through slot 30 and party data marked written (`party_data`, suites 20261001T220327-suite and 20261001T222547-suite). Sharing it with members: see "Needs the owner" | fire SetPartyMemberString; assert which slot is reached and that PartyUpdateMemberRequest leaves | |
| 20 | Social groups (guild lobby group) | Nodes SocialGroupsSetActive, OpenSocialGroupLink, SocialModMsgShown; Events onsocialgroupsupdated, onsocialgroupschangedactive | Shown at login | **works in scenario**: groups arrive, the active one is named, and the set-active node's path runs for it (`social_groups`, suites 20261001T220327-suite and 20261001T222547-suite); changing the group is not exercised, since it rewrites the account's guild group (profile key social\|group) | add an assert line to each scenario; then fire SocialGroupsSetActive | |
| 21 | Friend status text ("In lobby", ...) | Slot 52 FriendStatusString | Cosmetic | **needs the owner** (see "Needs the owner"): stub (empty string); no server source for a friend's status text | inject presence, assert the slot's text | none |
| 22 | Recently met players | Node RefreshRecentlyMetUsers; Slots 56-67; Expression R15NetIsRecentlyMetUserExpression | Rarely used; also a way to find someone to add (#6) | **needs the owner** (see "Needs the owner"): stub (empty); slot 57 is reached by the node (0x14019b870), no server source for the list | needs a server source; then fire the refresh and assert the slots | none |
| 23 | Remove a friend, in game | Msg FriendRemoveRequest/Response (Nakama :314) | Owner wants all features | **absent in the game's UI**: no remove-friend node is registered, no Social slot sends it, and the only code that does (pnsrad's CNSRADFriends) is never looked up by the game. Removal is the website's job today; making it in-game needs new UI, the owner's call | none until there's an entry point; the inbound side (FriendRemoveNotify) is #8 | **Destructive** |
| 24 | Decline a friend request, in game | Msg FriendRejectNotify (notify only) | Owner wants all features | **absent**: no node, no slot, and the protocol has no reject *request* (sns_friends.go defines only the notify); Nakama has no handler for one | none possible without a protocol addition | |
| 25 | Block a player | Nakama's blocked friend state (server/website only) | Owner wants all features | **absent in the game**: no node, slot, SNS message or event. The server can store a block (an invite to a blocked user fails, evr_pipeline_friends.go) but nothing in the game sets one | none possible in game | |
| 26 | Voice mute (user, lobby member, self) | Nodes VoipMuteUser/LobbyMember/Self; Events onvoipmuted, onvoipunmuted | Players use it | **works in scenario** (self and user): onvoipmuted/onvoipunmuted and the mute flag; a user muted through the mute-user node's handler (0x1401cc930) joins the game's mute list and leaves it on unmute, the account's own list untouched (`voip_mute`, run 20261001T220106 and suites 20261001T220327-suite and 20261001T222547-suite). Lobby-member mute ends in the same handler plus a per-slot component flag (0x140d11c10); that flag's path is not scenario-tested (its component lookup 0x1404f37a0 is not traced) | fire VoipMuteUser; assert onvoipmuted and the mute state expression | local only |
| 27 | Enable/disable social features | Node EnableSocialFeature (run 0x140ddb650 -> 0x14016afc0) | Gates other features | **works in scenario**: the node clears and sets its feature in the game's mask (`social_features`, suites 20261001T220327-suite and 20261001T222547-suite) | read the handler first; then a scenario per feature it gates | |
| 28 | Request another player's profile | Node RequestProfile (run 0x140de01f0); Caller 0x14019f510 ("Profile received for '%s'") | Shown when looking at a player | **works in scenario**: a friend's profile requested by the node arrives ("Profile received") (`request_profile`, suites 20261001T220327-suite and 20261001T222547-suite). It never did before cd62b75: every reply after the lobby join went to a closed game connection | fire RequestProfile(F); assert the request and "Profile received" | |

## The scenario suite

`just scenario-all` builds the scenario preset and runs every scenario in `tools/scenario/scenarios/`
unattended, one client at a time, and prints one PASS/FAIL table (`tools/scenario/run_all.py`;
suite.md and suite.json in the suite folder). It waits for 4096 MiB of free GPU memory and for the
previous wineserver to exit before each launch. At a10e631 it passed twice in a row, 16/16 each:
`/var/tmp/work-nevr-runtime/scenario-runs/20261001T220327-suite` and `.../20261001T222547-suite`.
The scenarios cover rows 1-9, 11-13, 17-20 and 26-28 (`invite`, `add_friend`, `friend_changes`,
`party_invite_accept`, `party_invite_dismiss`, `party_invite_join`, `party_join_errors`,
`party_members`, `party_join_by_id`, `party_lock`, `party_ui_buttons`, `party_data`, `social_groups`,
`social_features`, `voip_mute`, `request_profile`).

Defects found on the way, each fixed and committed: accept was not pnsovr's join (9bfed8c); join
failure codes lost (0496f65); members and invite senders shown as account ids (e62867d); the party
lock never reached the server (d2283ee); swapped slot labels 39/40, 42/43 (d2283ee); replies on the
login session sent to a closed game connection after the lobby join (cd62b75); six verify sensors
racing through `| grep -q` (9d4945a); the runner launching a client over a dying wineserver
(92e3197). In nakama, local only: an accept with no matching invite got no answer (a8eb2de79); every
join refusal was code 2, so "party full" never showed (f808932a8).

## Needs the owner (measured 2026-10-01; what has since been built is in the Nakama proposal's "As built" notes)

Each of these is blocked on a choice, not on work. The evidence says what the game and pnsovr do
and what Nakama has; the recommendation is one option, not a decision.

| # | What is missing | Evidence | Options | Recommendation |
|---|---|---|---|---|
| 10 | An in-game party does not make its members matchmake together | Nakama follows a party through a lobby group named by the user setting `LobbyGroupName` (evr_lobby_parameters.go:336-339, `JoinPartyGroup` evr_lobby_group.go), a different object from the SNS party the game's party UI makes (evr_pipeline_party.go). The game's find (R15NetFindMatchNode 0x140ddb740 -> 0x14016afc0 -> Find 0x140168500) lists party members as entrants (0x1401889d0), which Nakama does not read (evr/match_session_find_request.go `Entrants`). Testing it also puts the test client into real matchmaking | (a) Nakama joins the SNS party's members to one lobby group (the SNS party uuid as the group name) when they search; (b) read `Entrants` from the find request; (c) leave it | **needs the owner** (see "Needs the owner"): Nakama's party follow uses a lobby group from a user setting, not the in-game party, so the game's party never matchmakes together; and a test enters real matchmaking
| 18 | A friend's party id (slots 54 FriendIsJoinable, 55 FriendPartyId) | pnsovr parsed it from the Oculus presence deeplink (roster +0x78, filled by 0x180085a30). Nakama's SNSFriendStatusNotify is {FriendID, StatusCode} (evr/sns_friends.go:175); nothing carries a friend's party | (a) a new friend-presence message with party id and status text; (b) widen FriendStatusNotify (breaks old clients) | **works in scenario** (join by id): the party-join node reaches slot 2, the request goes out and the real server's "not found" comes back (`party_join_by_id`, suites 20261001T220327-suite and 20261001T222547-suite). A friend's party id (slots 54/55): see "Needs the owner"
| 21 | Friend status text (slot 52) | pnsovr returned the friend's Oculus presence string (0x1800850e0, pool +0x448), which each game publishes through SyncRichPresence (0x1401b9510). The game cuts it at the first '\|' (FUN_140da5d80). Nakama carries online/busy/offline only | as #18 | **needs the owner** (see "Needs the owner"): stub (empty string); no server source for a friend's status text
| 22 | Recently met players (slots 56-67) | pnsovr's were getters over ovr_User_GetLoggedInUserRecentlyMetUsersAndRooms (slot 57 0x180091070); nothing in the game feeds them. Nakama has nothing (`grep -i recentlymet` hits only hash names) | (a) Nakama records who shared a match and answers a new request; (b) the client records lobby entrants locally | **needs the owner** (see "Needs the owner"): stub (empty); slot 57 is reached by the node (0x14019b870), no server source for the list
| 19 | Party and member data reaching other members | The game's writes land (party_data.yaml). pnsovr then shared them from its Update (0x1800ac240 -> slots 6/7, Oculus room data store). Nakama's PartyUpdateRequest/UpdateMemberRequest carry no data and only rebroadcast a notify (evr/sns_party.go:104-148, evr_pipeline_party.go:717-733) | (a) give those requests a JSON payload and forward it in the notifies; (b) leave party data local | **works in scenario** (the game's writes): member data through slot 30 and party data marked written (`party_data`, suites 20261001T220327-suite and 20261001T222547-suite). Sharing it with members: see "Needs the owner"
| 17 | Join policy (invite-only, friends, friends of members, everyone) | Slot 16 stores it (party_lock.yaml); pnsovr set the Oculus room policy (import 0x1801fa5d0). Nakama has lock/unlock (open/closed) and no policy | (a) a policy field on the party, enforced on join; (b) map invite-only to locked | **works in scenario** (lock): the lock node makes the party unjoinable at once, the server locks it (PartyLockRequest, LockSuccess) and unlock reverses it (`party_lock`, suites 20261001T220327-suite and 20261001T222547-suite); slots 4/5 and pnsovr's host sync were missing, so a lock never reached the server (fixed d2283ee). Join policy: slot 16 stores it (same run); a server-side policy needs the owner
| 13 | What the "invitable users" button (slot 38) opens on PC | pnsovr opened the Oculus overlay (ovr_Room_LaunchInvitableUserFlow, 0x18008fe40); slots 39-43 were already bare `ret` in pnsovr | (a) open the game's own friends list; (b) nothing | **works in scenario** (routing): every mode of the node reaches its slot, 38, 40/39, 43/42 (`party_ui_buttons`, suites 20261001T220327-suite and 20261001T222547-suite); 39-43 do nothing, as pnsovr's bare `ret`. The slot labels 39/40 and 42/43 were swapped (fixed d2283ee). What slot 38 should open on PC: see "Needs the owner"

## In-game: party join failure codes (the owner plays, the harness forges)

**Why.** The game client knows six join-failure reasons. `PartyJoinFailedCB` (echovr.exe
0x140189590) logs `[NETGAME] Party join failed: %s` with the text from `GetJoinErrorString`
(0x1401b6530) and dispatches one script event per code:

| Code | Log text | Script event |
|---|---|---|
| 1 | not found | `delegate_onpartyjoinerrornotfound` |
| 2 | timeout | `delegate_onpartyjoinerrorunknown` (code 2 has no event of its own) |
| 3 | no permission | `delegate_onpartyjoinerrornopermission` |
| 4 | locked | `delegate_onpartyjoinerrorlocked` |
| 5 | full | `delegate_onpartyjoinerrorfull` |
| 6 | version | `delegate_onpartyjoinerrorversion` |
| other | unknown | `delegate_onpartyjoinerrorunknown` |

What each event shows the player is in the game's scripts, which have not been read. This test
records it. It also settles what the game service should send for "refused, no reason given".
Today it sends 2, and the bridge rewrites 2 to 4 (`GameJoinFailureCode`,
src/runtime/compat/social_party.h), so that case shows as "locked".

**Setup**
1. Build the test DLL: `just preset=mingw-scenario build`. The control endpoint exists only in
   this build.
2. Put `build/mingw-scenario/bin/BugSplat64.dll` in the game install as `BugSplat64.dll`, and
   start the game the way you normally play.
3. Find the control port in the game log:
   `[NEVR.SCENARIO] control listening on 127.0.0.1:<port> (test build only)`.
4. Go into a social lobby and open the party page on the tablet.
   - Measured earlier: outside a lobby the game logs `Game boot invite failed: %s` and dispatches
     no event (the callback checks netgame+0x2b08). What that field means is not traced.

**A. Through the bridge**

This is what a real failure from the game service does: it goes through the bridge's Feed and its
code mapping.

```
tools/scenario/control.py --port <port> '{"op":"inject","msg":"PartyJoinFailure","party":0,"code":N}'
```

Use N = 1, 3, 4, 5, 6 (these pass through unchanged), then 2 (shows as 4) and 9 (shows as 0).

**B. Straight to the game's callback**

This skips the bridge mapping, so you see the game's own handling of every code:

```
tools/scenario/control.py --port <port> '{"op":"fire","action":"party_join_failed_callback","code":N}'
```

Use N = 0, 1, 2, 3, 4, 5, 6, 7.

**Record, per injection**
- what the screen shows, with a screenshot;
- the game log lines `[NETGAME] Party join failed: <text>` and
  `[NEVR.SOCIAL] party callback index=2 arg=<N> bound=1`.

Every command and reply is also appended to `/var/tmp/work-nevr-runtime/control-log.jsonl`.

**Already measured.** The log text for path B matches the table above for codes 0–7: scenario
`party_join_failed_codes`, run 20261002T132250, 19/19. The bridge mapping in path A is covered by
`party_join_errors`. What is still open is what the player sees.

The runtime's log filter folds a repeated identical line into `[NEVR.LOGFILTER] repeated
count=...`. If you send the same code twice in a row, the second callback line is folded; record the
screen anyway.

**Results, 2026-10-02.** The owner watched while the harness sent the codes. The game was this
build (`v4.0.0-151-g891b510`, mingw-scenario) on the nested display, logged in to production, in a
social lobby, with the tablet's party page open. Client run `client-run-20261002T151652`; screenshots
in `/var/tmp/work-nevr-runtime/ingame-shots/`. Every failure opens the same popup, titled
**PARTY JOIN ERROR**, with one line of text and an OK button. A new failure replaces an open popup.

| Path | Code sent | Code the game saw | Log text | Popup text |
|---|---|---|---|---|
| A | 1 | 1 | not found | PARTY NOT FOUND |
| A | 3 | 3 | no permission | YOU DON'T HAVE PERMISSION TO JOIN |
| A | 4 | 4 | locked | PARTY IS UNJOINABLE |
| A | 5 | 5 | full | PARTY IS FULL |
| A | 6 | 6 | version | PARTY VERSION MISMATCH |
| A | 2 | 4 | locked | PARTY IS UNJOINABLE |
| A | 9 | 0 | unknown | PARTY JOIN ERROR UNKNOWN |
| B | 0 | 0 | unknown | PARTY JOIN ERROR UNKNOWN |
| B | 1 | 1 | not found | PARTY NOT FOUND |
| B | 2 | 2 | timeout | PARTY JOIN ERROR UNKNOWN |
| B | 3 | 3 | no permission | YOU DON'T HAVE PERMISSION TO JOIN |
| B | 4 | 4 | locked | PARTY IS UNJOINABLE |
| B | 5 | 5 | full | PARTY IS FULL |
| B | 6 | 6 | version | PARTY VERSION MISMATCH |
| B | 7 | 7 | unknown | PARTY JOIN ERROR UNKNOWN |

Code 2 has no popup of its own: the game logs "timeout" and shows the unknown popup, which matches
its event (`delegate_onpartyjoinerrorunknown`). The game server idled the client out at 20:23:38,
three seconds after the last code (`Kicked from server due to inactivity`); that was not caused by the test.

**Decide afterwards.** What the game service sends for "refused, no reason given". Today it
sends 2, the bridge rewrites it to 4, and the player sees PARTY IS UNJOINABLE. The candidates, by
what the player would see:
- keep 2→4: PARTY IS UNJOINABLE;
- send a code outside 1–6 and drop the rewrite: PARTY JOIN ERROR UNKNOWN;
- send 2 straight through: PARTY JOIN ERROR UNKNOWN on screen, "timeout" in the game log.

## Calibration (one human click, once)

The invite scenario enters at the friend row's script node action (0x140dddf60, verified from
its raw instructions: NetGame from the script context, SNSUserID on its input string, then the
handler posted on the deferred queue, nothing else). It cannot see what is above the node: the
input string the real row binds, which NetGame the script context yields, and whether the button
reaches the node. Since 6a99d5f the three nodes (send invite 0x140dddf60, add friend
0x140dd90f0, respond to invite 0x140dddd30) log their input once per press (`[NEVR.PARTY] node
<name> run input=...` / `index= accept=`), so one real click by a person gives a line to diff
against the scenario's fire line. The click itself has not happened yet. Every "entry: to
measure" scenario gets the same treatment: find the node, trace it, calibrate once.

## The other 18 UserProviderID callers

7160561 changed what `UserProviderID` returns (RAD to OVR), so every caller now sees code 4
instead of 0. The table is from a ReVault read (decompile and caller lists; the call graph covers
54816 of 55267 functions). Rows marked *raw* were checked against the instructions. The 0x647c8
object is the social object, from NetGame+0x647c8.

| Caller | What it does (evidence) | Uses the provider how | Exercised by |
|---|---|---|---|
| 0x14018aa90 friend invite handler (*raw*) | the friend row's invite | gate: silent return on mismatch | #3 (invite scenario) |
| 0x140187230 (*raw*) | same three gates, then social slot 39 OpenNewSendInviteUI | gate, silent | #13 |
| 0x1401873f0 | same, then slot 42 OpenPartyUI | gate, silent | #13 |
| 0x1401870f0 | AddFriend handler: gate, then slot 37 OpenFriendRequestUI | gate, silent | #6 add friend |
| 0x14016bee0 | gate, then a lookup on the social object, -1 on mismatch | gate, silent -1 | unknown purpose; #11 likely (member lookup) |
| 0x140199e70 | gate, then another social-object lookup | gate, silent -1 | unknown purpose |
| 0x1401776d0 | "is this user in the party", used by UpdatePersonalBubble/UpdatePersonalSpace | gate, silent false | #11 (party members get personal-space treatment); not asserted yet |
| 0x140177e50 | user lookup used by CR15NetGame::TeardownDisconnectedUser | raw symbol compare (no CSymbol64), silent false | #11 leave path |
| 0x14018a230 | index of a user in the party | raw symbol compare, silent -1 | #11 |
| 0x14019f510 | "Profile received for '%s'", display-name mismatch report, then a party-member event | raw symbol compare in its tail, silent return | #5/#11, #28 (member profile); not asserted yet |
| 0x14018a3c0 PartyMemberJoinedCB | "'%s' joined party", cancels and restarts matchmaking | builds an id, no gate | #11 |
| 0x14018a5c0 PartyMemberLeftCB | "'%s' left party", same matchmaking cancel | builds an id, then a lookup | #11 |
| 0x1401cbd00 | player id list for a lobby (callers: Create, LobbyRegistrationSuccessCB) | writes code into ids | #10 |
| 0x1401889d0 | id list for all party members (callers: Create, Find) | inline map to code, writes | #10 |
| 0x14018b540 | "%s purchased" (store) id list incl. party members | inline map, writes | none planned (store) |
| 0x14016be40, 0x14018a110, 0x140199dd0 | index-to-id helpers on the social object | write code into ids | indirectly by #5/#11 |
| 0x140d9d520 | script node: the local user's id string | writes code into an id | every scenario (local user id) |

Read across, the fix is consistent: every compare now sees 4 on both sides for OVR-ORG ids, and
every id the game builds for party members now carries 4 instead of 0, matching what Nakama
issues. What no scenario covers yet: the personal-bubble check (0x1401776d0), the profile tail
(0x14019f510) and the two unidentified lookups (0x14016bee0, 0x140199e70). #11 and #5 are the
scenarios that would reach them; their assertions should include the game's own log lines
("joined party", "Profile received") once those scenarios exist.

## Build order

1. Node traces: done (6a99d5f). The calibration click is still open (Spritz is asking the owner).
2. #4 receive, #5 accept, #12 dismiss: done in scenario (190941, 191139). #9 and #11: done (193721, 193907).
3. #6/#7 add a friend: implement slot 37 as a friend request, then the scenario; with #8 so the
   result shows up.
4. #9 join errors and #11 member join/leave: injection only.
5. #10 party into match: needs its entry point measured; touches real matchmaking.
6. #12, #13, #17-#22, #26-#28 as their handlers are read.
7. #14-#16: after the owner names a test friend.
8. #23-#25: need a decision from the owner (new UI, a protocol addition) before anything can be built.

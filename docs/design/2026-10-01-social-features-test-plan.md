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
| R15NetInviteUsersNode | 0x140ddc700 | posts 0x140187170, 0x140187230, 0x140187330, 0x1401873f0, 0x1401874f0 | 0x140187230 -> slot 39 OpenNewSendInviteUI (raw); 0x1401873f0 -> slot 42 OpenPartyUI; the other three to read |
| R15NetPartyRespondToInviteNode | 0x140dddd30 | 0x140188bf0, 0x140188f40 | to read (accept / dismiss) |
| R15NetPartyJoinNode | 0x140ddd3a0 | calls 0x140189570 | to read |
| R15NetPartyLeaveNode | 0x140ddd790 | posts 0x1401899f0 | to read |
| R15NetPartyKickNode | 0x140ddd610 | posts 0x140189910 | to read |
| R15NetPartyPassOwnershipNode | 0x140ddda20 | posts 0x14018aa00 | to read |
| R15NetPartyLockNode | 0x140ddd870 | 0x140189f20 | to read |
| R15NetPartySetJoinPolicyNode | 0x140dde110 | 0x14018ab90, 0x1401769e0 | to read |
| R15NetPartySetUnjoinableLobbyNode | 0x140dde2c0 | posts 0x14018abd0 | to read |
| R15NetPartyRefreshInvitesNode | 0x140dddb90 | posts 0x14018aa50 | to read (slot 69 RefreshInvites expected) |
| R15NetRefreshFriendsNode | 0x140ddf9a0 | posts 0x14019ae10 | expected slot 45 RefreshFriends (slot 45 is seen called in runs 143852 and 09-30; the handler itself is to read) |
| R15NetRefreshRecentlyMetUsersNode | 0x140ddfcc0 | posts 0x14019b870 | to read (slot 57 expected) |
| R15NetVoipMuteUserNode / MuteLobbyMemberNode / MuteSelfNode | 0x140de6110 / 0x140de5eb0 / 0x140de5fc0 | 0x1401b2350 (mute user) | voice subsystem, not Social |
| R15NetSetParty{Flag,Int,Real,String,Symbol}Node, R15NetSetPartyMember{Flag,Int,Real,String,Symbol}Node | 0x140de2640 ... 0x140de34a0 | to read | party / member shared data (slots 6/7 expected) |
| R15NetSocialGroupsSetActiveNode, R15NetOpenSocialGroupLinkNode, R15NetSocialModMsgShownNode | 0x140d8eba0, 0x140d8e430, 0x140de4a90 | to read | social groups (guild lobby group) |
| R15NetEnableSocialFeatureNode | 0x140ddb650 | posts 0x14016afc0 | to read |
| R15NetRequestProfileNode | 0x140de01f0 | to read | another player's profile |
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
| 4 | Receive a party invite (it appears in the invite list) | Node PartyRefreshInvites; Slots 70-72; Event onpartyinvitereceived; Msg PartyInviteNotify | The other half of #3 | unknown | setup: roster with F online. inject: PartyInviteNotify{party P, inviter F}. assert: InviteCount=1, InviteSender(0)=F, event onpartyinvitereceived | none (inbound only) |
| 5 | Accept a party invite (join the party) | Node PartyRespondToInvite (run 0x140dddd30); Slots 73, 2; Event onpartyjoined, onpartymemberjoined; Msg PartyJoinRequest/Success/Failure | Completes the invite loop | unknown | after #4: fire PartyRespondToInvite(accept). assert: PartyJoinRequest carries P; inject PartyJoinSuccess [F, self]; assert events onpartyjoined, onpartymemberjoined, MemberCount=2, MemberName(1)=F's name | joining moves the local user's real party; not destructive to anyone else |
| 6 | Add a friend, in game | Node AddFriend (run 0x140dd90f0 -> 0x1401870f0); Slot 37 OpenFriendRequestUI; Msg FriendInviteRequest/Success/Failure | Owner named it; the way friends are made inside the game | **works in scenario** since 0acbeb9: slot 37 sends SNSFriendInviteRequest (run 20261001T160330: slot 37 reached, FriendInviteRequest target=F, Nakama answered FriendInviteFailure for the synthetic F); a real click UNVERIFIED | implement slot 37 as SNSFriendInviteRequest(target account) (the message Nakama handles at evr_pipeline_friends.go:115). Scenario: fire AddFriend for synthetic F; assert slot 37 reached and FriendInviteRequest left with target F; inject FriendInviteSuccess; assert the outcome is shown (event or roster) | sends a real friend request if F is real; synthetic F |
| 7 | Accept a friend request, in game | Node AddFriend on the requester; Msg FriendAcceptSuccess, FriendAcceptNotify | Half of #6 | absent as its own action: no accept-friend node. Nakama turns an add to someone who already requested you into an accept (evr_pipeline_friends.go: "Mutual add" -> SNSFriendAcceptSuccess, and SNSFriendAcceptNotify to the other side), so #6 is accept too | as #6 with a pending request from F: assert FriendInviteRequest leaves and the server answers FriendAcceptSuccess. Not yet run: it needs a real pending request from another account, which a synthetic F cannot make; the client half (FriendAcceptSuccess triggers the roster refresh) passes in friend_changes | as #6 |
| 8 | Friend changes appear without a restart (added, accepted, removed, rejected, withdrawn) | Msg FriendInviteNotify, FriendAcceptNotify, FriendAcceptSuccess, FriendRemoveNotify, FriendRejectNotify, FriendWithdrawnNotify | Feedback for #6/#7, and the 09-30 owner report (a friend added on the website doesn't show) | **works in scenario** since 0acbeb9: every friend-change message triggers a friend-list refresh and the server's list rebuilds the roster (run 20261001T160446: FriendAcceptNotify and FriendRemoveNotify each sent FriendListRefreshRequest, Nakama answered FriendListResponse) | `tools/scenario/scenarios/friend_changes.yaml` | none (inbound only) |
| 9 | Party join errors are shown, not swallowed | Events onpartyjoinerror{full,locked,nopermission,notfound,version,unknown}; Msg PartyJoinFailure | A failed join that says nothing is the 09-30 bug class again | unknown | after #4 and the accept: inject PartyJoinFailure with each code. assert: the matching onpartyjoinerror* event, no member change | none |
| 10 | Party follows the leader into a match | Slots 31-34 (EnterLobby, ExitLobby and forwarders); Node PartySetUnjoinableLobby; Event onlobbyjoinerrorpartychange, onnetgamemenupartyjoined; Callers 0x1401cbd00, 0x1401889d0 | The point of a party is playing together | unknown | setup: a party of [self, F] by injection (#5). do: start matchmaking (entry to measure: the matchmaking node). assert: EnterLobby slot calls, and that the lobby request lists both members' ids with code 4 | touches real matchmaking on production |
| 11 | Party member joins or leaves (callbacks, UI roster) | Events onpartymemberjoined, onpartymemberleft, onpartymemberupdated; Callers PartyMemberJoinedCB, PartyMemberLeftCB; Msg PartyJoinNotify, PartyLeaveNotify | Every party changes membership | unknown | setup: party [self]. inject: PartyJoinNotify(F), then PartyLeaveNotify(F). assert: game logs "joined party" / "left party" for F, events fire, MemberCount 2 then 1 | the matchmaking-cancel branch in both callbacks fires if a search is running (game behaviour) |
| 12 | Dismiss (decline) a party invite | Node PartyRespondToInvite(dismiss); Slot 74; Msg PartyInviteResponse (param 0) | Common, cheap | unknown | after #4: fire PartyRespondToInvite(dismiss). assert: PartyInviteResponse(param 0) to F, InviteCount=0 | sends a decline to F's account; synthetic F |
| 13 | Invite users / party UI buttons | Node InviteUsers (run 0x140ddc700); Slots 39-43 | Buttons in the menus | stub; 0x140187230 -> slot 39 and 0x1401873f0 -> slot 42 confirmed, three more handlers to read | fire InviteUsers; assert which slots are reached; then decide what each should do (pnsovr opened the Oculus overlay here: owner's design call) | |
| 14 | Leave the party | Node PartyLeave (run 0x140ddd790 -> 0x1401899f0); Slot 17; Event onpartyleft; Msg PartyLeaveRequest/Success | Every party ends | unknown | setup: party [self, F] by injection. fire PartyLeave. assert: PartyLeaveRequest, event onpartyleft, MemberCount=1 | **Destructive** on a real party. Allowed only on an injected party; BLOCKED for real parties |
| 15 | Kick a member | Node PartyKick (run 0x140ddd610 -> 0x140189910); Slot 19; Event onpartykicked; Msg PartyKickRequest/Notify | Party leader tool | unknown | setup: party [self, F], self host. fire PartyKick(F). assert: PartyKickRequest targets F's member UUID | **Destructive**: BLOCKED until a test friend is named |
| 16 | Promote (pass ownership) | Node PartyPassOwnership (run 0x140ddda20 -> 0x14018aa00); Slot 18; Event onpartyhostchanged; Msg PartyPassRequest/Notify | Party leader tool | unknown | as #15; assert PartyPassRequest, then inject PartyPassNotify and assert IsHost=0 | **Destructive** on a real party: BLOCKED |
| 17 | Lock/unlock the party, join policy | Nodes PartyLock, PartySetJoinPolicy; Slots 16, 21; Event onpartyjoinabilitychanged; Msg PartyLock/Unlock* | Less used | unknown | fire PartyLock; assert Lock request, Joinable flips, event onpartyjoinabilitychanged | changes a real party's policy; reversible |
| 18 | Join a friend's party from the friends list | Node PartyJoin (run 0x140ddd3a0); Slots 54 FriendIsJoinable, 55 FriendPartyId | Shortcut players expect | stub (both slots return 0, so the option never shows) | first implement from presence data; then fire PartyJoin for F's party; assert PartyJoinRequest | needs a server source for a friend's party id |
| 19 | Party and member shared data | Nodes SetParty*/SetPartyMember*; Slots 6 PushMemberData, 7 ShareData; Msg PartyUpdate*, PartyUpdateMember* | Used by menus for party state | stub (slots 6/7 do nothing; inbound PartyUpdateNotify/UpdateMemberNotify are parsed in `src/runtime/compat/social_party.h`) | fire SetPartyMemberString; assert which slot is reached and that PartyUpdateMemberRequest leaves | |
| 20 | Social groups (guild lobby group) | Nodes SocialGroupsSetActive, OpenSocialGroupLink, SocialModMsgShown; Events onsocialgroupsupdated, onsocialgroupschangedactive | Shown at login | works (every run: "Social lobby group info received", the active group named) | add an assert line to each scenario; then fire SocialGroupsSetActive | |
| 21 | Friend status text ("In lobby", ...) | Slot 52 FriendStatusString | Cosmetic | stub (empty string) | inject presence, assert the slot's text | none |
| 22 | Recently met players | Node RefreshRecentlyMetUsers; Slots 56-67; Expression R15NetIsRecentlyMetUserExpression | Rarely used; also a way to find someone to add (#6) | stub (empty) | needs a server source; then fire the refresh and assert the slots | none |
| 23 | Remove a friend, in game | Msg FriendRemoveRequest/Response (Nakama :314) | Owner wants all features | **absent in the game's UI**: no remove-friend node is registered, no Social slot sends it, and the only code that does (pnsrad's CNSRADFriends) is never looked up by the game. Removal is the website's job today; making it in-game needs new UI, the owner's call | none until there's an entry point; the inbound side (FriendRemoveNotify) is #8 | **Destructive** |
| 24 | Decline a friend request, in game | Msg FriendRejectNotify (notify only) | Owner wants all features | **absent**: no node, no slot, and the protocol has no reject *request* (sns_friends.go defines only the notify); Nakama has no handler for one | none possible without a protocol addition | |
| 25 | Block a player | Nakama's blocked friend state (server/website only) | Owner wants all features | **absent in the game**: no node, slot, SNS message or event. The server can store a block (an invite to a blocked user fails, evr_pipeline_friends.go) but nothing in the game sets one | none possible in game | |
| 26 | Voice mute (user, lobby member, self) | Nodes VoipMuteUser/LobbyMember/Self; Events onvoipmuted, onvoipunmuted | Players use it | unknown; goes to the voice subsystem (0x1401b2350), not Social | fire VoipMuteUser; assert onvoipmuted and the mute state expression | local only |
| 27 | Enable/disable social features | Node EnableSocialFeature (run 0x140ddb650 -> 0x14016afc0) | Gates other features | unknown | read the handler first; then a scenario per feature it gates | |
| 28 | Request another player's profile | Node RequestProfile (run 0x140de01f0); Caller 0x14019f510 ("Profile received for '%s'") | Shown when looking at a player | unknown (the facade already sends OtherUserProfileRequest for friend names) | fire RequestProfile(F); assert the request and "Profile received" | |

## Calibration (one human click, once)

The invite scenario enters at the friend row's script node action (0x140dddf60, verified from
its raw instructions: NetGame from the script context, SNSUserID on its input string, then the
handler posted on the deferred queue, nothing else). It cannot see what is above the node: the
input string the real row binds, which NetGame the script context yields, and whether the button
reaches the node. Next slice adds a trace on the node (its input string, NetGame and xpid), so
that one real click by a person gives a line to diff against the scenario's. Every "entry: to
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

1. Node trace on the friend row's invite node and the calibration click (Spritz is asking the
   owner for it).
2. #4 receive and #5 accept a party invite: inbound plus one fire, nothing destructive.
3. #6/#7 add a friend: implement slot 37 as a friend request, then the scenario; with #8 so the
   result shows up.
4. #9 join errors and #11 member join/leave: injection only.
5. #10 party into match: needs its entry point measured; touches real matchmaking.
6. #12, #13, #17-#22, #26-#28 as their handlers are read.
7. #14-#16: after the owner names a test friend.
8. #23-#25: need a decision from the owner (new UI, a protocol addition) before anything can be built.

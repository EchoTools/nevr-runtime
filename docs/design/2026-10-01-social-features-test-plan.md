# Social features: prioritized list and how each is tested

2026-10-01. Companion to `docs/design/2026-10-01-social-scenario-harness.md` (the harness this
list runs on). Owner's order: "create a prioritized list of all social features and how it's
going to test them."

## How the list was built (complete by construction)

Every feature below comes from one of four sources, named in the **Source** column:

- **Slot N**: the game's Social interface, as implemented by the facade
  (`src/runtime/patch/social_facade_object.cpp`, the `kVtable` table: 75 slots, each traced).
- **Event**: a `delegate_on*` session event name from the game's symbol table
  (`src/runtime/log/symcache_data.cpp`; 104 delegate names, 29 of them social).
- **Msg**: an SNS wire message the game's protocol defines (same symbol table; party and
  friend message families).
- **Caller**: one of the 19 echovr.exe callers of `CNSProvider::UserProviderID` (0x1406186d0),
  mapped in the last section.

Status vocabulary: **works** = seen working in a run log, with the run named; **broken** =
seen failing; **stub** = the facade slot returns a constant or does nothing; **unknown** = never
exercised. "Seen" means a log line, not a belief.

A scenario enters the game only through the game's own entry point (the design doc's rule).
Where that entry point is not yet measured, the plan says **entry: to measure**. Measuring it
(ReVault, then raw disassembly) is the first step of building that scenario.

**Destructive** = changes a real account's friendships or a real party (remove, kick, leave or
promote in a real party). These stay **BLOCKED** until the owner names a test friend.

## Priority list

Ordered by what players hit first and most often.

| # | Feature | Source | Why this rank | Status (evidence) | Scenario: setup / do / assert | Risk |
|---|---|---|---|---|---|---|
| 1 | Friends list: names, online/offline, order | Slots 45-51; Msg FriendListResponse, FriendStatusNotify | Every session opens the friends tab | works (2026-09-30 owner run: names and presence shown; scenario 143852 roster state) | setup: inject FriendListResponse + FriendStatusNotify for 3 ids (2 online). do: entry: to measure (the friends tab's open/refresh script). assert: slots FriendCount=3, OnlineFriendCount=2, online first, FriendName per index | name lookups go to production Nakama for real ids |
| 2 | Party exists after login (auto create) | Slot 13 Update bit0; Event onpartycreated; Msg PartyCreateRequest/Success | Prerequisite for every party feature | works (scenario 143852 step 3: party id 23, joinable) | already step 3 of the invite scenario | none |
| 3 | Send a party invite to a friend | Slot 8; Caller 0x14018aa90; Msg PartyInviteRequest | The reason parties exist | works in scenario after 7160561 (143852: SendInvite, PartyInviteRequest target=F); a real click UNVERIFIED (see "Calibration") | the invite scenario (`tools/scenario/scenarios/invite.yaml`); its diagnose list should also name the events onpartyinviteerror{offline,uninvitable,unjoinable,unknown,firstmatchnotcompleted} | the request reaches production; synthetic target |
| 4 | Receive an invite (it appears in the invite list) | Slots 70-72; Event onpartyinvitereceived; Msg PartyInviteNotify | The other half of #3 | unknown | setup: roster with F online. inject: PartyInviteNotify{party P, inviter F}. assert: InviteCount=1, InviteSender(0)=F, event onpartyinvitereceived | none (inbound only) |
| 5 | Accept an invite (join the party) | Slot 73, Slot 2; Event onpartyjoined, onpartymemberjoined; Msg PartyJoinRequest/Success/Failure | Completes the invite loop | unknown | after #4: do: entry: to measure (the invite list's accept script). assert: PartyJoinRequest carries P; inject PartyJoinSuccess [F, self]; assert events onpartyjoined, onpartymemberjoined, MemberCount=2, MemberName(1)=F's name | joining moves the local user's real party; it's not destructive to anyone else |
| 6 | Join errors are shown, not swallowed | Events onpartyjoinerror{full,locked,nopermission,notfound,version,unknown}; Msg PartyJoinFailure | A failed join that says nothing is the 09-30 bug class again | unknown | after #4 and the accept: inject PartyJoinFailure with each code. assert: the matching onpartyjoinerror* event, no member change | none |
| 7 | Party follows the leader into a match | Slots 31-34 (EnterLobby, ExitLobby and forwarders); Event onlobbyjoinerrorpartychange, onnetgamemenupartyjoined; Caller 0x1401cbd00 (lobby id list), 0x1401889d0 (Create/Find) | The point of a party is playing together | unknown | setup: a party of [self, F] by injection (#5). do: start matchmaking (entry: to measure). assert: EnterLobby slot calls, and that the lobby request lists both members' ids with code 4 | touches real matchmaking on production |
| 8 | Party member joins or leaves (callbacks, UI roster) | Events onpartymemberjoined, onpartymemberleft, onpartymemberupdated; Callers 0x14018a3c0 PartyMemberJoinedCB, 0x14018a5c0 PartyMemberLeftCB; Msg PartyJoinNotify, PartyLeaveNotify | Every party changes membership | unknown | setup: party [self]. inject: PartyJoinNotify(F), then PartyLeaveNotify(F). assert: game logs "joined party" / "left party" for F, events fire, MemberCount 2 then 1 | the matchmaking-cancel branch in both callbacks fires if a search is running (that's game behaviour, not ours) |
| 9 | Dismiss (decline) an invite | Slot 74; Msg PartyInviteResponse (param 0) | Common, cheap | unknown | after #4: do: entry: to measure. assert: PartyInviteResponse(param 0) to F, InviteCount=0 | sends a decline to F's real account if F is real; use a synthetic F |
| 10 | Newly added friend appears without a restart | Msg FriendInviteNotify, FriendAcceptNotify, FriendRemoveNotify, FriendRejectNotify, FriendWithdrawnNotify | Owner-reported 09-30 (friend added on the website doesn't show) | broken by construction: the roster handles only FriendListResponse and FriendStatusNotify (`src/runtime/compat/social_roster.h` Feed) | inject FriendAcceptNotify(F), assert FriendCount+1 (fails today); then fix the feed | none |
| 11 | Leave the party | Slot 17; Event onpartyleft; Msg PartyLeaveRequest/Success | Every party ends | unknown | setup: party [self, F] by injection. do: entry: to measure. assert: PartyLeaveRequest, event onpartyleft, MemberCount=1 | **Destructive** on a real party. Allowed only on an injected party; BLOCKED for real parties |
| 12 | Kick a member | Slot 19; Event onpartykicked (the kicked side); Msg PartyKickRequest/Notify | Party leader tool | unknown | setup: party [self, F], self host. do: entry: to measure. assert: PartyKickRequest targets F's member UUID | **Destructive**: BLOCKED until a test friend is named |
| 13 | Promote (pass ownership) | Slot 18; Event onpartyhostchanged; Msg PartyPassRequest/Notify | Party leader tool | unknown | as #12, assert PartyPassRequest, then inject PartyPassNotify and assert IsHost=0 | **Destructive** on a real party: BLOCKED |
| 14 | Lock/unlock the party (join policy) | Slots 16, 21; Event onpartyjoinabilitychanged; Msg PartyLock/Unlock* | Less used | unknown | do: entry: to measure. assert: Lock/Unlock request, Joinable flips, event onpartyjoinabilitychanged | changes a real party's policy; reversible |
| 15 | Join a friend's party from the friends list | Slots 54 FriendIsJoinable, 55 FriendPartyId | Shortcut players expect | stub (both return 0, so the option never shows) | first implement from presence data; then the scenario as #5 entered from the friend row | needs a server source for a friend's party id |
| 16 | Friend status text ("In lobby", ...) | Slot 52 FriendStatusString | Cosmetic | stub (empty string) | inject presence, assert the slot's text | none |
| 17 | Invite-send UI, party UI, friend-request UI buttons | Slots 37-43 (Open*UI); Callers 0x1401870f0 (slot 37), 0x140187230 (slot 39), 0x1401873f0 (slot 42) | They're buttons in the menus | stub; before 7160561 the same provider mismatch silently dropped these calls too (raw disassembly of 0x140187230: identical three gates, then `call [vtable+0x138]`) | fire the gate function's script node (entry: to measure; its caller is node 0x140ddc700). assert: the slot is reached (trace logs the first calls) | pnsovr opened the Oculus overlay here; what our version should do is a design decision for the owner |
| 18 | Recently met players | Slots 56-67 | Rarely used | stub (empty) | not planned until there's a server source | none |
| 19 | Social groups (guild lobby group) | Events onsocialgroupsupdated, onsocialgroupschangedactive | Shown at login | works (every run: "Social lobby group info received", the active group named) | already visible in every scenario log; add an assert line | none |
| 20 | Friend add / accept / decline / remove from inside the game | Msg FriendInviteRequest, FriendAcceptRequest, FriendRemoveRequest | Players manage friends on the website | **not exposed by the game's Social interface**: no slot sends these (the only friend-management slot is 37 OpenFriendRequestUI, a UI overlay). Done on the website; the client only receives the Notify messages (#10) | none in-game; #10 covers the client side | remove is destructive; not reachable from the game anyway |
| 21 | Blocking players | none found | | **not exposed**: no slot, no event, no SNS message in the symbol table | none | |
| 22 | Voice mute | Events onvoipmuted, onvoipunmuted | | local voice feature, not the social service; out of scope here | | |
| 23 | Party member data sharing | Slots 6 PushMemberData, 7 ShareData; Msg PartyUpdate*, PartyUpdateMember* | | stub; inbound PartyUpdateNotify/UpdateMemberNotify are parsed (`src/runtime/compat/social_party.h`) | unknown until the game is seen calling slots 6/7 | |

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
| 0x140187230 (*raw*) | same three gates, then social slot 39 OpenNewSendInviteUI | gate, silent | #17 |
| 0x1401873f0 | same, then slot 42 OpenPartyUI | gate, silent | #17 |
| 0x1401870f0 | gate, then slot 37 OpenFriendRequestUI | gate, silent | #17 |
| 0x14016bee0 | gate, then a lookup on the social object, -1 on mismatch | gate, silent -1 | unknown purpose; #8 likely (member lookup) |
| 0x140199e70 | gate, then another social-object lookup | gate, silent -1 | unknown purpose |
| 0x1401776d0 | "is this user in the party", used by UpdatePersonalBubble/UpdatePersonalSpace | gate, silent false | #8 (party members get personal-space treatment); not asserted yet |
| 0x140177e50 | user lookup used by CR15NetGame::TeardownDisconnectedUser | raw symbol compare (no CSymbol64), silent false | #8 leave path |
| 0x14018a230 | index of a user in the party | raw symbol compare, silent -1 | #8 |
| 0x14019f510 | "Profile received for '%s'", display-name mismatch report, then a party-member event | raw symbol compare in its tail, silent return | #5/#8 (member profile); not asserted yet |
| 0x14018a3c0 PartyMemberJoinedCB | "'%s' joined party", cancels and restarts matchmaking | builds an id, no gate | #8 |
| 0x14018a5c0 PartyMemberLeftCB | "'%s' left party", same matchmaking cancel | builds an id, then a lookup | #8 |
| 0x1401cbd00 | player id list for a lobby (callers: Create, LobbyRegistrationSuccessCB) | writes code into ids | #7 |
| 0x1401889d0 | id list for all party members (callers: Create, Find) | inline map to code, writes | #7 |
| 0x14018b540 | "%s purchased" (store) id list incl. party members | inline map, writes | none planned (store) |
| 0x14016be40, 0x14018a110, 0x140199dd0 | index-to-id helpers on the social object | write code into ids | indirectly by #5/#8 |
| 0x140d9d520 | script node: the local user's id string | writes code into an id | every scenario (local user id) |

Read across, the fix is consistent: every compare now sees 4 on both sides for OVR-ORG ids, and
every id the game builds for party members now carries 4 instead of 0, matching what Nakama
issues. What no scenario covers yet: the personal-bubble check (0x1401776d0), the profile tail
(0x14019f510) and the two unidentified lookups (0x14016bee0, 0x140199e70). #8 and #5 are the
scenarios that would reach them; their assertions should include the game's own log lines
("joined party", "Profile received") once those scenarios exist.

## Build order

1. Node trace on 0x140dddf60 and the calibration click (Spritz is asking the owner for it).
2. #4 receive and #5 accept: inbound only, no destructive step, completes the invite loop.
3. #6 join errors and #8 member join/leave: injection only.
4. #10 friend notifications: a known-broken item with a cheap fix.
5. #7 party into match: needs its entry point measured; touches real matchmaking.
6. #11-#14: after the owner names a test friend.

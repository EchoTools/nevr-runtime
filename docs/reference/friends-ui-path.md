# Friends and party UI: what runs `R15NetRefreshFriendsNode`

How the friends refresh is wired in `echovr.exe` and the script modules, so a windowed-client
run can be pointed at the UI that triggers it. Everything below is static (ReVault and the
shipped script DLLs); nothing here was observed on a running client except where marked.

## The node

| Fact | Value | Source |
| --- | --- | --- |
| Node class name | `R15NetRefreshFriendsNode` | string at `0x141cc7f78` (echovr.exe); the bytes before it, from `0x141cc7f68`, are the hash qword in the next row |
| Node type hash (CSymbol64) | `0x4f0a3c282e9f3b43` | the qword at `0x141cc7f68`; recomputed from the name with `src/abi/symbol_hash.h` |
| Update function | `0x140ddf9a0` (409 bytes) | ReVault `fn show 0x140ddf9a0` |
| Registered by | `0x140f4c480` (the node-type registration table): `0x140f505f1` loads `0x140ddf9a0` as the node's function and calls `0x1412ec220` | ReVault xrefs to `0x140ddf9a0`: `0x140f505f1`, plus data at `0x141f92f44`, `0x141f92f54`, `0x142180f60` |
| Direct callers | none | the only call-graph edge into `0x140ddf9a0` is the registration table; so the node runs from the script interpreter, not from C++ (inferred from the call graph; the graph misses 451 of 55267 functions) |

What the update function does (decompiled, `0x140ddf9a0`): it is a three-state coroutine node.
State 0 binds the node. State 1 fetches the NetGame (`GetNetGameFromContext`, `0x140113a90`) and
posts the handler `0x14019ae10` onto its queue (`0x140198460(netgame + 0x2b20, netgame, 0x14019ae10)`).
State 2 waits until that queued request has been consumed (compares the queue counter at
`+0x1f0` with the recorded position, then `0x14019ba60`). The handler `0x14019ae10` is the
`RefreshFriends` slot 45 of the social facade (`src/runtime/patch/social_facade_object.cpp`,
`TRACED(45, RefreshFriends)`; `src/runtime/scenario/scenario_control.cpp`
`kRefreshFriendsHandlerVA`). On our side that slot sends `FriendListRefreshRequest`
(`SocialParty::RefreshFriends`, `src/runtime/compat/social_party.h`).

## Which script runs the node

Script modules are the hash-named DLLs under `echovr/bin/win10/scripts/`. A byte search of every
file in that directory for the node hash (little-endian `43 3b 9f 2e 28 3c 0a 4f`) finds exactly
three modules, each at two offsets:

| Module | Offsets | Reconstruction | What it is |
| --- | --- | --- | --- |
| `e9b0db765f1eb096.dll` | `0x1fcf`, `0x646e` | `echovr-reconstruction/src/scripts/e9b0db765f1eb096.cpp` | Category "social". Handles `evt_refresh_friends` and the `delegate_onparty*` / `delegate_onlobbymember*` delegates. Its node set is the friends list UI: `R15NetFriendsExpression`, `R15NetFriendExpression`, `UIInfiniteScrollExpression`, `GetCanvasElementChildAtIndexExpression`, `SetText2Node`, `SetSpriteUINode`, `ShowUINode`, `R15NetPartyExpression`, `R15NetPartyMemberExpression`, `R15NetLobbyExpression`, `R15NetIsPartyMemberExpression`, `R15NetRefreshFriendsNode` |
| `d74afbc03c66a45e.dll` | `0x9bf` (registration), `0x15fc` | `.../d74afbc03c66a45e.cpp` | Category "spectator". Handles `delegate_onpage2enabled`; its nodes are `R15UIPage2EnablePageNode`, `R15UIPage2EnabledExpression`, `R15NetGameExpression` (reads `loggedin`), `R15NetRefreshFriendsNode` |
| `732f980d8193e3f5.dll` | `0x142f`, `0x2261` | `.../732f980d8193e3f5.cpp` | Category "audio"; handles `evt_boot_sequence_finished`; also contains `R15NetBeginLoginNode`, `R15UIPage2EnablePageNode` and the node |

The hashes of the trigger names, computed with the repo's CSymbol64 code
(`serverdb` reproduces `0x25e886012ced8064`):

| Name | Hash |
| --- | --- |
| `evt_refresh_friends` | `0x561287f0028ef7b1` |
| `delegate_onpage2enabled` | `0xd64f3478683e4a9c` |

`0x561287f0028ef7b1` occurs only in `e9b0db765f1eb096.dll` (two places, `0xb41c` and `0x155c2`).
No other file under `echovr/bin/win10` posts it, and neither `revault search code` nor
`revault search instructions` finds it in `echovr.exe`.

## What this says about the UI

- The friends list is drawn by the social script `e9b0db765f1eb096`, and the refresh is requested
  through its `evt_refresh_friends` event.
- `d74afbc03c66a45e` ties a refresh to a page being enabled (`delegate_onpage2enabled`) while
  logged in: its node-transition table runs the logged-in check (`0xe44da977`) between the
  page-enable delegate (`0x95d4d232`) and the nodes after it.
- The event is posted by something that is not a plain file: the `.rad` UI layout/resource that
  holds the tablet's tabs lives in the packed `echovr/_data` resources, which a byte search of
  the install cannot read. The tab or button that posts `evt_refresh_friends` is therefore not
  named by this trace.

## Not proven

- Which tablet tab/button posts `evt_refresh_friends`. Static candidates: opening the friends tab
  on the arm-computer tablet, or a page-2 enable. To settle it, drive the windowed client to the
  tablet, open each tab in turn and look for slot 45 in the run's log (the social facade traces
  every slot call: `TRACED(45, RefreshFriends)`).
- Which of the three node-table entries in `d74afbc03c66a45e` is the refresh node (the reconstruction
  names its transitions by id, not by class).

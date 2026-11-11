# Join hold: the level-load functions it reads

The join hold holds a dedicated server's lobby accepts while a session's level set is loading. It reads three
functions of `echovr.exe`. This page records them, the server chain that reaches them, what was read and what was
inferred, and the live run that tested its prediction. The three are pinned in `src/runtime/hook/addresses.h` (section
"Level-load state the join hold reads") and in `tools/hook_identity_manifest.json`. With `network.join_hold_ms` above 0 on a dedicated server, `src/runtime/server/join_hold.cpp`
detours two of them (`LoadLevel` and `EnableGameSpaces`); `Enable` is validated and never hooked. With the key at 0, the default, nothing is hooked.

The binary is `echovr.exe`, sha256 `b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043`, 35397120
bytes, image base 0x140000000. The ReVault `echovr` project holds the same file. Addresses here are VAs. `addresses.h`
stores RVAs, the form of its neighbours; the manifest stores VAs.

Tags on the chain: **[R]** read in ReVault decompilation or bytes; **[I]** inferred, with the basis given; **[N]** not read.

## The hold

`network.join_hold_ms` (config.yaml, section `network`) is the longest wait, in milliseconds. 0, the default, is off: no hook is
installed and every accept is forwarded at once. A value from 1 to 499 is raised to 500 and one above 15000 is lowered to it, each with a
Warning; text that is not a whole number is off with a Warning. A client never installs it.

What is held is the server's forward of the service's accept (`LobbyAcceptPlayersSuccessV2`, `kLobbyEntrantsAccept` in
`src/runtime/server/gameserver_callbacks.cpp`): a copy of the encoded bytes is queued with its arrival time and no game function is
patched. The failure message is not held. Accepts go to the game in arrival order, and once one is queued every later one queues behind it.

The gate (`src/runtime/server/join_hold_rules.h`, `Gate`):

- A session create arms it, expecting a load. A `LoadLevel` entry counts a load in flight and closes it; a `LoadLevel` exit counts it done.
- An `EnableGameSpaces` return opens it only when no load is in flight and a load was seen since the arm, so a return left over from the
  lobby cannot open it during the session's own load. It then stays closed for 200 ms after the last activity (`kSettleMs`).
- At `join_hold_ms` after the front accept arrived, everything is released anyway (Warning `reason=timeout`) and the gate disarms: it fails
  open, and it also ends a load count stuck by a `LoadLevel` that never returned.
- The service's `CODE_ENDED`, `EndServerSession` and the next session create drop what is held (Warning `dropped=<n>`). A reconnect's
  `EndSession` does not.

A session whose levels are already loaded calls no `LoadLevel`; its accepts wait for the timeout.

Both prologues are checked before either detour goes in. A mismatch on either logs `join hold not armed ... reason=prologue_mismatch`
(Warning, expected and actual bytes), attaches nothing and leaves the game untouched. If a detour then fails to attach after its prologue
matched (`reason=detour_failed`), the detour that did go in is removed again (`PatchRetire`: the record in HookGuard first, then
`MH_RemoveHook`, which writes the game's bytes back) and logs `join hold hook removed` (Info). If that removal fails, the Warning
`join hold hook NOT removed` says the detour stays installed: it only forwards to the game's own function and feeds a gate no one
uses (`g_active` is false), so the game behaves as stock, but its bytes are hooked. Either way the hold is not armed and accepts
are forwarded at once. This is not `ServerFatal`: the hold is optional.

Per accept, the `EnableGameSpaces` return and each release, `[NEVR.GAMESERVER] join hold ...` lines carry counts and the session id.
A release logs `released accept` only when the accept was handed to the game. With no broadcaster or no send function it logs the
Warning `dropped accept` instead, with the entrant count, and nothing was delivered.

## The three functions

| Constant | VA | Size | Name basis | What the binary says | Prologue (8 bytes) |
| --- | --- | --- | --- | --- | --- |
| `CNCAGAMESPACE_LOAD_LEVEL` | 0x1404FE050 | 2322 | ReVault record (source imported). Its reconstruction row is marked stale in ReVault. `docs/engine/asset-system.md` gives the same name to this address. | `[LEVELLOAD] Loading level '%s'` (string 0x1417207A0, code reference 0x1404FE0F4); `[LEVELLOAD] Finished loading level '%s' in %llu ms` (string 0x1417207F8, code reference 0x1404FE8E6) | `488bc44889580848` |
| `CGAMELEVELRESOURCE_ENABLE` | 0x1404ED4B0 | 1422 | reconstruction row at confidence M | `[LEVELLOAD] Enabling level '%s'` (string 0x141720928, code reference 0x1404ED4FD) | `4055535657415441` |
| `CR15NETGAME_ENABLE_GAME_SPACES` | 0x140161940 | 947 | Quest symbol of the structural twin; no PC symbol [I] | `user[%llu]\|spawned` (string 0x1416DB338, code reference 0x140161C16 inside it); the function logs no line of its own | `40555357488dac24` |

Each code reference lies inside the function named in its row.

## The server chain

The steps run in this order on a dedicated server. Each VA is a function entry unless a step says otherwise.

1. **`BeginMultiplayerLobby`, 0x1400FDDD0 [R].** The runtime calls it at server start (`src/runtime/lifecycle/state_machine.cpp`); its name is set in `src/abi/echovr_functions.cpp`. It calls `BeginMultiplayer` (0x1400FDA70).
2. **Server flags.** The comment on `SERVER_FLAGS_CHECK` in `addresses.h` records that `PatchEnableServer` writes `OR QWORD ptr[rax], 6` at 0x1401580C3, inside `fcn.140157fb0`, which sets bits 1 and 2 of the flag word. The reconstruction (`src/reconstruction/r15/cr15_net_game_begin_multiplayer.cpp`) reads bit 2 as "a dedicated server". The patch code itself was not read [N]; the bit reading is the reconstruction's [I], and steps 4, 9, 11 and the BeginMultiplayer branch use it.
3. **`CR15NetGame::LobbyJoinAcceptedCB`, 0x14017C810 [R].** Its callees include `IsOwner`, `OwnerJson_GetBool`, `SetGameState` (0x1401B8650) and `LoadMap` (0x14017A050). When the gamespace chain at NetGame+0x2B10 is empty: if the local account is not the lobby owner and the lobby's `ingame` flag is clear, it calls SetGameState(6, "server loading"); otherwise it calls LoadMap.
4. **`CR15NetGame::Update`, 0x1401BBDB0 [R].** In state 6 with `ingame` set, it calls LoadMap. In state 7, once the count at Game+0x1F10 is zero, it switches state through SetGameState unless flag bit 21 or bit 42 is set; those bits branch to `fcn.1401A89C0` and `fcn.1401A89F0` instead, not read [N]. With no root gamespace at NetGame+0x2B10 it switches to state 5. With a root gamespace it switches to state 9 when flag bit 2 is set and to state 8 when it is clear (the step-2 reading [I]).
5. **`CR15NetGame::LoadMap`, 0x14017A050 [R].** It logs "Loading map '%s'", calls the game object's vslot 0x330 to create the root gamespace, stores it at NetGame+0x2B10, then calls SetGameState(7, "loading level"). The vslot 0x330 target at 0x140100680 calls 0x1404EB460; the rest of the route to `CreateGameSpace` is [N].
6. **Gamespace creation, `CncaGame::CreateGameSpace`, 0x1404EAC60 [R].** It inserts a `CGameSpaceInitFiber` into the array at Game+0x1EE0. The insert is the call at 0x1404EB369 to `fcn.1400d9480`, whose body was not read. The count of that array is at Game+0x1F10, the offset `GAME_DEFER_COUNT_OFFSET` in `addresses.h`. What the count means is [I], below.
7. **`CGameSpaceInitFiber` vslot[0], 0x14050B390 [R].** If flag bit 1 at fiber+0x100 is set, it does nothing. Otherwise, when bit 0 is clear it calls `LoadLevel` (0x1404FE050), and when bit 0 is set it calls the dynamic loader 0x1404FD650 (not covered below). Then, if the completion callback at fiber+0xF0 is non-null, it calls it with the value at fiber+0xF8, the value at fiber+0x30, and a flag that is set when the result check returns zero.
8. **`CncaGameSpace::LoadLevel`, 0x1404FE050 [R].** It logs the Loading and Finished lines. It enables nothing.
9. **`SetGameState`, 0x1401B8650 [R] (manifest name `EchoVR::NetGameSwitchState`).** It logs "NetGame switching state (from %s, to %s)" on every switch. For state 9 it calls `SetMemPool` (0x1400CECC0) and `BeginMultiplayerGameplay` (0x1400FDE20), then pushes the thunk 0x14013C660 onto the game's update queue through `queue` (0x140104E70). The name "exclusive update" for that queue is a ReVault comment that cites the Quest symbol [I]. Then it sets the owner flag `ingame` (`OwnerJson_SetBool`, 0x140615B50, value 1) only when `IsOwner` (0x14060D040) returns non-zero: the bytes at 0x1401B9170 to 0x1401B9177 are the call and `test eax,eax`, and `je` skips the set. Finally it fires the in-game delegate. The queue call is at 0x1401B8DD0; the push is the `lea` at 0x1401B8DB6 and its store at 0x1401B8DCB. After the queue call, the code calls 0x14015A600 only when flag bit 2 is clear, which is a client if step 2's reading holds: the test is at 0x1401B8DD5 to 0x1401B8DE1, and the call at 0x1401B8DE9 is skipped when the bit is set. This case-9 code lies beyond the 1060-byte extent of the function record, in a separate chunk.
10. **Queue thunk, 0x14013C660 [R].** Its five bytes are `jmp 0x140161940`.
11. **`CR15NetGame::EnableGameSpaces`, 0x140161940 [R].** When NetGame+0x2B10 is null, the jump at 0x140161960 goes to the epilogue at 0x140161CE8: no gamespace is walked and no entrant loop runs [R]. Otherwise it walks the chain at NetGame+0x2B10 through +0x18 and calls `EnableGameSpace` (0x1404EF130) for each gamespace. It then runs a loop over the entrants, 0x140161B46 to the `jc` at 0x140161CC2: the index starts at the 16-bit value at NetGame+0xDE, is compared with the 16-bit value at NetGame+0xE0 (0x140161B26 and 0x140161CBB) and goes up by one at 0x140161CB8 [R]. The format call and the `OwnerJson_SetBool` call that write the owner flag `user[%llu]|spawned`, at 0x140161C28 and 0x140161C99, lie inside that loop, so the flag is written in the loop's body [R]. When NetGame+0x2B18 is set, it first calls fcn.1404ED260 (the call at 0x140161976, before the walk) [N]. Its return is the level-set-complete marker this page uses.

    `EnableGameSpace` searches the sorted array at Game+0x1F90 (count at Game+0x1FC0). Each entry is a 16-byte pair: a key and an object. It takes the entry whose key equals the value at +0x58 of the gamespace it was given, which is [I] as a reading of that field. On a match it calls `Enable` (0x1404ED4B0) on the entry's second word. Then, whatever `Enable` returned, it calls `OutputError` (0x14151EE70) with that result and 8, then `fcn.14011F5F0`, `fcn.1404FA170`, the game object's vslot 0x378 [N for its target], and `fcn.1404E8AC0`. `EnableGameSpace` does not branch on `Enable`'s result. `OutputError` does: for a first argument of 0 or -1 it returns 0 at once (0x14151EE8D to 0x14151EE9A [R]). Other values take a path that was not read [N].

    `Enable` returns at once, with no effect, unless bit 7 of the word at +0xC40 of its object is set [R]. When that bit is set, `Enable` clears it, sets bit 8, and writes "[LEVELLOAD] Enabling level" only when the global at 0x1416CA4F0 is non-zero. That global is in the read-only section `.rdata` and holds 1, so the line is written whenever bit 7 is set [R].

    Whether every gamespace in the chain is enabled by the return therefore depends on the key lookup, on bit 7, and on the queue having run, which are [I].

Steps 9 to 11 explain why the marker comes after state 9: the function runs from the queue, not at the state change.

The name in row 3 rests on structure, not on a PC symbol. The offsets (NetGame+0x2B10, +0x2B18, +0x18, +0x2DA0, +0xDE, +0xE0, the 0x250 entrant stride), the string, and the event constant 0x17B3C424DE4DCD68 passed to `fire` (0x1401AA4E0) match the Quest function of that name.

## Inferred, not read

- **[I] Game+0x1F10 counts the in-flight load fibers, and reaches zero only after the last Finished loading level line.** The basis is the gate in `CR15NetGame::Update` (step 4) and the insert at step 6. The code that removes fibers from the array is [N]. `addresses.h` (`GAME_DEFER_COUNT_OFFSET`) says the entries' identity is not established; step 6 reads them as init fibers. This inference governs where the Finished lines fall.
- **[I] The state-9 switch line comes after the last Finished line, and the Enabling lines come after the switch.** The Enabling lines come from `Enable`, which `EnableGameSpace` calls from `EnableGameSpaces` in the queue after the switch (steps 9 to 11).
- **[I] The key lookup and bit 7.** Only an entry whose key matches the gamespace's value at +0x58, and whose object has bit 7 set at +0xC40, is enabled. The live run below may test this; the other causes are listed under Prediction.
- **[I] The names.** Row 3's name is the Quest symbol carried over by the structural match. Row 2's name is a reconstruction at confidence M. Row 1's name is ReVault's imported record, which the engine notes in `docs/engine/asset-system.md` also use.

## Not read

- The case-7 handler, 0x1401A1680 (state 7's own work).
- `fcn.1401A89C0` and `fcn.1401A89F0`, which step 4's bits 21 and 42 branch to instead of a switch.
- The body of 0x14015A600, which case 9 calls only when flag bit 2 is clear (step 9). Only its prologue and the bit test that gates the call were read.
- The body of `fcn.1400d9480`, the insert called at 0x1404EB369 (step 6), and the code that removes fibers from the array at Game+0x1EE0.
- The virtual-call route from vslot 0x330 to `CreateGameSpace`, beyond the call from 0x140100680 to 0x1404EB460.
- The game object's vslot 0x378 target, and `fcn.1404E8AC0`, which `EnableGameSpace` calls after `Enable` (step 11).
- The path of `OutputError` (0x14151EE70) for a first argument other than 0 or -1. The calls after it in `EnableGameSpace` run only if it returns.
- fcn.1404ED260, which `EnableGameSpaces` calls when NetGame+0x2B18 is set. The reconstruction names it `DisableGameSpace`.
- `fcn.14011F5F0` and `fcn.1404FA170`, called after `Enable` (step 11), and what they do to the matched entry.
- The meaning of the 16-bit values at NetGame+0xDE and +0xE0, the entrant loop bounds (step 11).
- The bodies of the other callers of `EnableGameSpace` (0x1404EF130) that the call graph records: fcn.140104240, FUN_140114240, fcn.140119EF0, fcn.140124E30, `LeaveGame` (0x1401796B0), fcn.1404FC6D0, FUN_140501580, fcn.140509D70, FUN_140518050 and FUN_1412AB390. Whether any of them runs on a dedicated server is open.
- `BeginMultiplayer`'s call to `EnableGameSpace` at 0x1400FDAE6. Its branch was read: the bytes at 0x1400FDAB1 to 0x1400FDAB9 load the flag word through an object pointer and `jnz` to 0x1400FDB6C, over that call, when bit 2 is set [R]. The object that pointer comes from was not identified [N].
- How the matchmaker or ServerDB session start reaches `LobbyJoinAcceptedCB`. `src/runtime/server` was not traced past `BeginMultiplayerLobby`.
- Whether a dedicated server reaches `WaitOnLevelLoads` (0x140128B90) with game flag bit 1 set, through vslot 43 (0x140128590).
- The patch code that `PatchEnableServer` uses to write the flag bits (step 2).
- Whether a `longjmp` out of `LoadLevel` (the game's crash recovery) can leave the gate's load count above zero. The detour counts the exit
  after the original returns, so such a load is never counted done. The count stays up, and a later session's accepts wait for the
  timeout (`join_hold_ms`) before they are released, one timeout late; the timeout resets the count and disarms the gate, and the next
  session create arms it again. No live run has produced this case [I].

## Candidates not used

Two other functions were considered for the level-set marker and rejected:

- `WaitOnLevelLoads`, 0x140128B90, walks the game's level-set table at Game+0x9A0 and calls `EnableGameSpace` (call at 0x140128DCD). It is not among the direct callees of `LobbyJoinAcceptedCB`. Its role as the Game-level set's wait is [I].
- The fiber's vslot[0], 0x14050B390 (step 7), calls `LoadLevel` for one fiber. The count at Game+0x1F10 is taken to count such fibers [I].

## Not covered

The dynamic-level loader at 0x1404FD650 logs "[LEVELLOAD] Loading dynamic level" and is outside this join hold.

## Live-run row

One dedicated-server run, 2026-11-11, from the commit `da584317c960efd9ca3a9cb41c6cd0163b2c31d8`. Its server log (quoted below as "server log, line N", with the timestamps of that date, UTC) is the row; the client that joined wrote its own log ("client log, line N").

**Build.** A scratch clone of that commit with a local tag `v6.17.0` and the forced release environment (`GITHUB_ACTIONS=true GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v6.17.0`), preset `mingw-plugin-dev`. The runtime carries `NEVR-BUILD 6.17.0 da584317c960efd9ca3a9cb41c6cd0163b2c31d8` and the host `NEVR-HOST-BUILD dev`. That commit has no join hold (`src/runtime/server/join_hold.cpp` does not exist in it), so the run shows the game's own order.

**Route.** The server ran from a throwaway game copy against the production game service, headless (`DISPLAY` and `WAYLAND_DISPLAY` unset), with the stored server sign-in. Its `config.yaml` set `network.regions` to a dev region (not `default`, so public matchmaking never offered it) and put the `novalidation` tag in the query of `services.serverdb_uri`, which the token route keeps (`Connecting to ServerDB at <socket host>/nevr?tags=novalidation&regions=<dev region>`, server log, line 502); the tag makes the service skip its reachability probe. `registration successful` (server log, line 510) followed. A session came from the service's allocate RPC (`echo_arena_private`, level `mpl_arena_a`, the dev region), and it started only when a client joined it: a second game copy ran `echovr.exe -windowed -mp -lobbyid <lobby id>` on a headless display. Allocation at 20:45:56 started nothing; the client's join at 20:47:58 did. The pattern is in AGENTS.md ("System test for runtime and auth changes").

**The server log, in order.**

| Line | Text |
| --- | --- |
| 491 | `[LEVELLOAD] Finished loading level 'r14_glb_global_mp' in 2155 ms` (boot, 20:45:47) |
| 543 | `[NEVR.GAMESERVER] session starting session_id=6db17c30-59af-48a7-af57-2bf8ba2e41fb` |
| 548-550 | `[NETGAME] Session Started`, `[NETGAME] Loading map 'mpl_arena_a'`, `[NETGAME] NetGame switching state (from lobby, to loading level)` |
| 552 | `[LEVELLOAD] Loading level 'mnu_master_mp_ingame'` |
| 554 | `[LEVELLOAD] Finished loading level 'mnu_master_mp_ingame' in 57 ms` (line 1) |
| 555 | `[LEVELLOAD] Loading level 'mpl_arena_a'` |
| 558, 563 | `[NSLOBBY] pending connection from [10.0.8.50:6794]` (the client), `[NETGAME] Entrant DMO-1525633280207098008 added to slot 0` |
| 577 | `[LEVELLOAD] Finished loading level 'mpl_arena_a' in 710 ms` (line 1) |
| 578 | `[NETGAME] NetGame switching state (from loading level, to in game)` (line 2) |
| 580 | `[LEVELLOAD] Disabling level 'mnu_master'` |
| 581, 582 | `[LEVELLOAD] Enabling level 'mpl_arena_a'`, `[LEVELLOAD] Enabling level 'r14_glb_global_mp'` (line 3) |

The session ended at 20:49:26 with `Destroy Peer 0 [10.0.8.50:6794] (communication timeout)` (server log, line 618): the client took 67759 ms to load `mpl_arena_a` under Wine (client log, line 691) and the server dropped it for the communication timeout 15 s after it reached in game.

**Prediction.** The first bullet applied: line 2 (server log, line 578) follows the last line 1 (line 577) and comes before line 3 (lines 581 and 582). No Finished line follows line 2 and no Enabling line precedes it. The run is consistent with the Game+0x1F10 inference and the queue ordering for this session. It says nothing about a session whose levels are already loaded.

**What the run corrected.**

- The lines the first draft expected before the row, `[NETGAME] LobbyJoinAcceptedCB` and a switch to loading level from it, are in the client's log: `LobbyJoinAcceptedCB` (client log, line 589), `NetGame switching state (from lobby, to server loading)` (line 591), `Loading map 'mpl_arena_a'` (line 602), `NetGame switching state (from server loading, to loading level)` (line 603). The server's log holds no `LobbyJoinAcceptedCB` line (0 matches). The server reaches loading level from `Session Started` (server log, lines 548 to 550). Step 3 of the chain is therefore the client's path; the server's route to `LoadMap` from a session start is not read [N]. The client's own sequence, `Finished` (client log, lines 607 and 691), `(from loading level, to ready for game)` (line 693), `(from ready for game, to in game)` (line 696), `Enabling` (lines 700 and 701), has the row's line 2 text only on the server: its state before in game is `ready for game`, not `loading level`.
- The two `Finished` lines and the two `Enabling` lines name different levels. `mnu_master_mp_ingame` finished (line 554) and was not enabled in this session; `r14_glb_global_mp` was enabled (line 582) after finishing at boot (line 491). Equal counts did not mean each finished level was enabled. Which `Enable` call took each level, and why `mnu_master_mp_ingame` was not enabled (the key lookup, bit 7, or `Disabling level 'mnu_master'` at line 580), is not established [I].

## Verification

- `python3 tools/verify_hook_invariants.py` checks the prologue of each row in `tools/hook_identity_manifest.json` against the game binary (`check_identity`). It does not compare the constants in `src/runtime/hook/addresses.h` with the rows, so an edit to a constant passes it.

# Empty-server TTL: holding the return to lobby

`network.empty_server_ttl_seconds` (config.yaml, flat key `nevr_empty_server_ttl_s`) is the number of
seconds a dedicated server keeps running after its session has no players. **Default 0: off, the
behaviour without the feature.** It stays opt-in until a live server run shows the game survives the hold.

## What it holds

When the game's session has no players, the engine calls `NetGameScheduleReturnToLobby`
(`echovr.exe 0x1401a89f0`, called from `Update 0x1401bbdb0` among others), returning to lobby unloads the
level, and `NetGameSwitchStateHook` (`src/runtime/lifecycle/state_machine.cpp`) then exits the process.
With a TTL above 0 the runtime detours that function (prologue-validated, boot-time config read in
`src/runtime/lifecycle/boot.cpp`) and `nevr_return_to_lobby::Request` (`src/runtime/lifecycle/return_to_lobby.cpp`)
holds a request made while no player session is accepted (join state 4,
`ServerContext::CountAcceptedEntrants`). The hold ends in one of three ways, decided in
`src/runtime/lifecycle/return_to_lobby_hold.h`:

- the TTL elapses: the swallowed return is issued;
- a player joins: the hold is dropped and the session continues;
- a shutdown is pending (Ctrl+C, a shutdown command): the hold is dropped and the process exits as usual.

The runtime's own call on a ServerDB `CODE_ENDED` goes through the same `nevr_return_to_lobby::Request`, so it
is held too. The failed-level-load reset in `state_machine.cpp` and the `NEVR_ScheduleReturnToLobby`
export call the game function directly and are never held. If the detour cannot be installed (prologue
mismatch) the TTL is dropped to 0 and nothing is held.

The game asks again every tick while the session stays empty (`CR15NetDedicatedLobby` vslot 1,
`0x1401bbb80`, state 0xb, calls `0x1401a89f0` and changes no state). Only the first request of a hold is
logged; the line that ends the hold carries the request count. After the TTL releases the return, requests
proceed until a player has been seen, so an empty session spends one TTL.

## What it does not do

The hold keeps the process alive. It does not keep the match joinable: Nakama ends an empty started
match on its own. In nakama's `server/evr_match.go`, `MatchLoop` counts `emptyTicks` while a started
match has no presences and after 60 s calls `MatchShutdown`, which closes the match, sends an entrant
reject and a `LobbySessionEvent` `CODE_ENDED` to the game server. A game-server-originated `CODE_ENDED`
(`server/evr_pipeline_lobby.go`, `SignalEndedSession`) only leaves the match stream. So within the hold the server is up but Nakama no longer routes
players to that match; making an empty server joinable for up to the TTL needs a Nakama change.

## Not verified

No live server run. `Update 0x1401bbdb0` also calls `0x1401a89f0` every frame while a game flag
(bit 42 of the lobby flags) is set, and `0x1401a89f0` only queues a callback; whether that flag path
can fire while a player has joined during a hold, and so return a session that has players, is
unobserved. The detour target's prologue was read from the dev install's `echovr.exe`
(`40 53 48 83 ec 30 33 d2`), and the decision logic is unit-tested, but whether the game re-issues the
request each frame while held, and what it does with a level that stays loaded and empty, is unobserved.

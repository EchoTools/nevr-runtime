# Empty-server TTL: holding the return to lobby

`network.empty_server_ttl_seconds` (config.yaml, flat key `nevr_empty_server_ttl_s`) is the number of
seconds a dedicated server keeps running after its session has no players. **Default 0: off, the
behaviour without the feature.** It stays opt-in until a live server run shows the game survives the hold.

## What it holds

When the game's session has no players, the engine calls `NetGameScheduleReturnToLobby`
(`echovr.exe 0x1401a89f0`, called from `Update 0x1401bbdb0` among others), returning to lobby unloads the
level, and `NetGameSwitchStateHook` (`src/runtime/lifecycle/state_machine.cpp`) then exits the process.
With a TTL above 0 the runtime detours that function (prologue-validated, boot-time config read in
`src/runtime/lifecycle/boot.cpp`) and `ReturnToLobby::Request` (`src/runtime/lifecycle/return_to_lobby.cpp`)
holds a request made while no player session is accepted (join state 4,
`ServerContext::CountAcceptedEntrants`). The hold ends in one of three ways, decided in
`src/runtime/lifecycle/return_to_lobby_hold.h`:

- the TTL elapses: the swallowed return is issued;
- a player joins: the hold is dropped and the session continues;
- a shutdown is pending (Ctrl+C, a shutdown command): the hold is dropped and the process exits as usual.

The runtime's own call on a ServerDB `CODE_ENDED` (`gameserver.cpp`) goes through the same
`ReturnToLobby::Request`, so it is held too.

## What it does not do

The hold keeps the process alive. It does not keep the match joinable: Nakama ends an empty started
match on its own. In nakama's `server/evr_match.go`, `MatchLoop` counts `emptyTicks` while a started
match has no presences and after 60 s calls `MatchShutdown`, which closes the match, sends an entrant
reject and a `LobbySessionEvent` `CODE_ENDED` to the game server. A game-server-originated `CODE_ENDED`
(`server/evr_pipeline_lobby.go`, `SignalEndedSession`) only leaves the match stream. So within the hold the server is up but Nakama no longer routes
players to that match; making an empty server joinable for up to the TTL needs a Nakama change.

## Not verified

No live server run: the detour target's prologue was read from the dev install's `echovr.exe`
(`40 53 48 83 ec 30 33 d2`), and the decision logic is unit-tested, but whether the game re-issues the
request each frame while held, and what it does with a level that stays loaded and empty, is unobserved.

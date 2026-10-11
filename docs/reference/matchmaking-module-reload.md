# `pnsradmatchmaking.dll`: where the game reloads it and where we patch it

The matchmaker host default inside `pnsradmatchmaking.dll` must be rewritten on every load of the
module, because the game unloads and reloads it during a session and a reload maps a fresh,
unpatched image. This records both halves from ReVault (`echovr.exe`) and the runtime source.

## The game side: load and unload

| Step | Address | What happens |
| --- | --- | --- |
| Trigger | `0x140157fb0` (NetGame state function), the lobby step near `0x140159979` | After the SYSNET check (`0x1401f6fa0`), if `0x14060c770(CNSLobby)` is false (the matchmaking interface pointer at `CNSLobby+0x28` is null), it reads the config string `matchmaking_plugin` (`0x1416dc130`, default `pnsradmatchmaking` at `0x1416dc118`) and calls `LoadMatchmakingSupport` |
| Load | `LoadMatchmakingSupport`, `0x14060b810` | logs `[NSLOBBY] loading matchmaking library '%s'`, `LoadLibrary` (`0x14105aa70`) into `CNSLobby+0x20`, `GetProcAddress("MatchmakingLib")` into `CNSLobby+0x28`, calls the interface initializer; on failure it calls `FreeLibrary` (`0x14105ae30`) and clears `+0x20` |
| Unload | `~CR15NetLobby`, `0x14014e540` -> `0x140603710` | `LeaveSession`, `CNSLobby::EndSession`, `CNSLobby::Unregister`, release of the interface at `+0x28`, then `FreeLibrary(CNSLobby+0x20)` (matchmaking) and `FreeLibrary(CNSLobby+0x30)` (a second module handle, released after its interface at `+0x38`) |
| Who destroys the lobby | `0x140150020` and `0x1401500c0` call `~CR15NetLobby`; ReVault's callers to depth 3 are `0x140145b30` and `0x140157fb0` (`BeginMultiplayer` and `0x14015a050`..`0x14015a500` reach `0x140157fb0`) | every lobby teardown is an unload, and the next lobby creation loads the module again |

So the reload in #18 is a property of the lobby object's lifetime: a new `CR15NetLobby` runs
`LoadMatchmakingSupport` again, and `FreeLibrary` drops the previous image when its reference count
reaches zero, so the next `LoadLibrary` maps new bytes at (possibly) a new base.

## The runtime side: where the patch is applied

| Piece | Where |
| --- | --- |
| Registration | `nevr_pnsrad_enabler::Init` registers `OnDllLoaded` with `LdrRegisterDllNotification` (`src/runtime/patch/pnsrad_enabler.cpp`, "Patch 4") |
| Per-load handler | `OnDllLoaded` in the same file: for a load notification whose `BaseDllName` is `pnsradmatchmaking.dll` it calls `PatchMatchmakingHost(DllBase)` with no one-shot guard (the `pnsrad.dll` branch below it does use `s_pnsradPatched`) |
| The patch | `PatchMatchmakingHost`: reads `GetMatchmakerBridgePort()` (`src/runtime/compat/ws_bridge.cpp`) at call time, builds `ws://127.0.0.1:<port>`, `memcmp`s the 47 characters at RVA `0x1c84d8` (a 48-byte slot: the 47 characters and their NUL, `nevr_matchmaker_host_patch::kHostSlotSize`) against `wss://matchmaker.readyatdawn.com/rad/rad15_live`, then `PatchMemory`. A slot that does not match is left alone and logged as `reason=bytes_mismatch` at Warning |

Because the handler keys on the notification and not on a flag, a reload that maps a new image is
patched again. A load that maps the same already-patched image fails the `memcmp` and logs the
`reason=bytes_mismatch` warning; that line alone is not the failure from #18 (see below).

## What a run's log must show

The two counts below are emitted by different components and must be equal for a run in which the
module loaded N times:

- game: `[NSLOBBY] loading matchmaking library 'pnsradmatchmaking'` (level 2, from `0x14060b810`)
- runtime: `[NEVR.PATCH] pnsradmatchmaking patched matchmaker host default at +0x1c84d8` (Info)

A load without a following patch line, or a `matchmaker listener never bound a port` warning, is the
failure from #18. A `pnsradmatchmaking host patch skipped ... reason=bytes_mismatch` warning is the
same failure only when the slot holds the original `wss://matchmaker.readyatdawn.com/...` text (the
`actual=` field); with the patched `ws://127.0.0.1:<port>` text it is a re-load of an image that is
already patched.

## Not proven

- That the module is in fact unloaded and reloaded on a real PC session (the original #18 report is
  the only observation). The path above shows when it would be: a lobby object teardown followed by a
  new lobby.
- That a reload patched by this path populates the PUBLIC MATCH screen; that needs the kiosk check
  from a client run.
- The `LdrRegisterDllNotification` registration is logged only at Debug
  (`pnsrad registered DLL notification`), so a run at Info level cannot show it succeeded; its
  failure paths log at Error.

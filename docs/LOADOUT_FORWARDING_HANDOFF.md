# Loadout Forwarding Handoff

## Goal
Forward cosmetic loadout updates from EchoVR's internal broadcaster to the game service via the TCP broadcaster proxy.

## Reverse-Engineering Findings (echovr.exe)
- Broadcaster init: `FUN_140157fb0` registers all message handlers.
- Save loadout handler: `OnMsgSaveLoadoutRequest` @ 0x1401a79d0
  - Message already contains the binary-serialized cosmetic loadout.
  - Slot encoding in first `uint32`: low 16 bits = slot (0–15), high 16 bits = generation/session id.
  - In-game loadout instances: `game + 0x51420 + slot * 0x40`; data pointer at offset 0x30.
- Current loadout response handler: `OnMsgCurrentLoadoutResponse` @ 0x14015f530
  - Binary EasyStream payload; magic `0xc8c33e4833671bbd`.
  - Same slot encoding as above.
- Symbols (hashes): see `GameServer/messages.h`.
  - TCP proxy (serverdb):
    - `SYMBOL_TCPBROADCASTER_SAVE_LOADOUT_REQUEST = 0x7777777777770B00`
    - `SYMBOL_TCPBROADCASTER_CURRENT_LOADOUT_RESPONSE = 0x7777777777770B01` (added)

## Current Code (nevr-server)
- Handlers: `GameServer/gameserver.cpp`
  - `OnMsgSaveLoadoutRequest`:
    - Extracts slot from low 16 bits of first uint32, validates < 8.
    - Forwards binary payload to `SYMBOL_TCPBROADCASTER_SAVE_LOADOUT_REQUEST` when `registered && sessionActive`.
  - `OnMsgCurrentLoadoutResponse`:
    - Extracts slot, validates, forwards to `SYMBOL_TCPBROADCASTER_CURRENT_LOADOUT_RESPONSE`.
- Symbols: `GameServer/messages.h` includes the new current-loadout-response symbol.
- Build: Release build succeeds; `GameServer.dll` deployed to game directory.

## Why This Is Correct
- Cosmetic data is not in `EntrantData.json`; it is in the binary loadout instance and serialized into the broadcaster message.
- The game-side handlers (RE) use standard x64 ABI; forwarding raw payload preserves cosmetic data.

## How to Test
1) Ensure game server is registered and `sessionActive`.
2) Trigger a loadout save in the customization UI.
3) Observe logs (from handlers): slot/genId/payload size and "Forwarded ... bytes" messages.
4) Verify downstream on the TCP proxy for these symbols:
   - `SYMBOL_TCPBROADCASTER_SAVE_LOADOUT_REQUEST`
   - `SYMBOL_TCPBROADCASTER_CURRENT_LOADOUT_RESPONSE`
5) For current loadout requests, expect analogous forwarding logs.

## Troubleshooting
- If forwarding does not occur, check:
  - `registered && sessionActive` flags.
  - Add temporary logs around `SendServerdbTcpMessage` in both handlers to confirm invocation and symbol id.
  - Verify TCP proxy logging for the two symbols.
- If cosmetics still fail to apply, hook deeper at `FUN_1401958f0` (apply stage) to inspect resolved cosmetic IDs.

## Key Paths
- Handlers: `GameServer/gameserver.cpp`
- Symbols: `GameServer/messages.h`
- Build output: `build/linux-wine-linux-wine-release/bin/GameServer.dll`

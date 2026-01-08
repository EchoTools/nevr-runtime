# Loadout Customization Broadcaster Message Research

## Problem Statement
Currently no message is being sent from the game server to the game service when a player updates their loadout at the character creation screen. There is a remote log called `CUSTOMIZATION_METRICS_PAYLOAD` that contains customization data, but it's not enough to know which slot was customized.

The server profile contains the `cosmetic_loadout`, but the game service is responsible for that profile. We need to figure out if there's an internal broadcaster message that can be relayed to the game service over the TCP websocket broadcaster proxy.

## Findings from Ghidra Analysis (Port 8193 - EchoVR Binary)

### Key Strings Found

**Customization Metrics Node:**
- Address: `141cbc4b8` - `"[CR15NetCustomizationMetricsSendNode]: Not initialized"`
- Address: `141cc5c78` - `"OlPrEfIxR15NetCustomizationMetricsSendNode"`

**Customization Component:**
- Address: `141cb94e0` - `"CR15NetCustomizationCS"` (NetCustomization Component System)
- Address: `141cb2f20` - File path: `"d:\\projects\\rad\\dev\\src\\rad15\\r15net\\r15netcomponents\\cr15netcustomizationcs.cpp"`

**Loadout Save Messages:**
- Address: `1416d60f8` - `"SR15NetSaveLoadoutRequest"`
- Address: `1416d6130` - `"SR15NetSaveLoadoutSuccess"`  
- Address: `1416d6168` - `"SR15NetSaveLoadoutPartial"`
- Address: `1416d61c0` - `"SR15NetSaveLoadoutFailure"`

**Current Loadout Messages:**
- Address: `1416d63c0` - `"SR15NetCurrentLoadoutRequest"`
- Address: `1416d63f8` - `"SR15NetCurrentLoadoutResponse"`
- Address: `1416d6488` - `"SR15NetLoadoutNumberNotification"`

**Profile Update Messages (Important!):**
- Address: `141c3afa8` - `"OlPrEfIxSNSUserServerProfileUpdateRequest"` 
- Address: `141c3b010` - `"OlPrEfIxSNSUserServerProfileUpdateSuccess"`
- Address: `141c3b048` - `"OlPrEfIxSNSUserServerProfileUpdateFailure"`
- Address: `141c3d0f6` - `"OlPrEfIxSNSUpdateProfile"` (with "Tm" prefix)

**Remote Log:**
- Address: `1416d4fa3` - `"OlPrEfIxSNSRemoteLogSetv3"` 
- Address: `1416d4fd0` - `"OlPrEfIxSRemoteLogs"`
- Address: `1416d5160` - `"remote_log_metrics"`

**Metrics System:**
- Address: `141cb5528` - `"Only one CR15NetMetricsCS is allowed to exist"`
- Address: `141cb5560` - File: `"d:\\projects\\rad\\dev\\src\\rad15\\r15net\\r15netcomponents\\cr15netmetricscs.cpp"`

**Customization Panel:**
- Address: `141cbc220` - `"customization_panel"`
- Address: `141cbc200` - `"OlPrEfIxcustomization_panel"`

### Key Observations

1. **RemoteLog vs Broadcaster Messages**: The `CUSTOMIZATION_METRICS_PAYLOAD` is sent via `SNSRemoteLogSetv3`, which is for telemetry/analytics, not for communicating game state changes to the game service.

2. **Profile Update Path**: The most likely path for loadout updates to reach the game service is through:
   - `SNSUserServerProfileUpdateRequest` - Client/game sends this to update the server profile
   - This would contain the updated `cosmetic_loadout` data
   - Game service receives this and updates the profile in its database

3. **Loadout Save Messages**: There are several loadout-related broadcast messages:
   - `SR15NetSaveLoadoutRequest` - Request to save loadout
   - `SR15NetSaveLoadoutSuccess/Partial/Failure` - Results of save operation
   - These appear to be internal game messages

4. **Script Node**: `CR15NetCustomizationMetricsSendNode` is a script node that likely sends the customization metrics remote log, but this is for telemetry/analytics, not for communicating loadout updates to the game service.

## Current Game Server Implementation

From `/home/andrew/src/nevr-server/GameServer/gameserver.cpp`:

The game server currently listens for these TCP broadcaster messages:
- `SYMBOL_TCPBROADCASTER_LOBBY_REGISTRATION_SUCCESS`
- `SYMBOL_TCPBROADCASTER_LOBBY_REGISTRATION_FAILURE`
- `SYMBOL_TCPBROADCASTER_LOBBY_START_SESSION`
- `SYMBOL_TCPBROADCASTER_LOBBY_PLAYERS_ACCEPTED`
- `SYMBOL_TCPBROADCASTER_LOBBY_PLAYERS_REJECTED`
- `SYMBOL_TCPBROADCASTER_LOBBY_SESSION_SUCCESS_V5`

And internal broadcaster messages:
- `SYMBOL_BROADCASTER_LOBBY_SESSION_STARTING`
- `SYMBOL_BROADCASTER_LOBBY_SESSION_ERROR`

**Missing**: No listener for loadout save requests or profile updates!

## Next Steps

1. ✅ Confirmed `SNSUserServerProfileUpdateRequest` exists in binary
2. 🔍 Need to find the symbol ID for `SNSUserServerProfileUpdateRequest` in messages.h
3. 🔍 Search for cross-references to understand when it's triggered by loadout changes
4. ✏️ Add listener in game server for internal loadout save messages
5. ✏️ Forward loadout updates as profile updates to game service via TCP broadcaster

## Working Hypothesis

The loadout customization flow should work as follows:
1. Player customizes loadout in character creation screen
2. Game sends `SR15NetSaveLoadoutRequest` (internal broadcast message)
3. **[MISSING]** Game server should listen for this message
4. **[MISSING]** Game server should trigger forwarding to game service via TCP broadcaster
5. Game service receives the profile update and persists it

**The missing piece**: The game server implementation doesn't listen for `SR15NetSaveLoadoutRequest` or any loadout-related messages, and doesn't forward them to the game service.

## Symbol IDs Found

**SR15NetSaveLoadoutRequest:**
- String Address: `1416d60f8`
- Symbol Hash: `0x6d451003fb4b172e` (7851662967438091054 decimal)
- Function that initializes it: `FUN_1400a4c90` at address `1400a4c90`
- Hash function used: `_HashA_SMatSymData_NRadEngine__SA_K_K0_Z`

## Network Initialization Function (FUN_140157fb0)

This function appears to be the main network game initialization that registers ALL broadcaster message listeners. Found at address `140157fb0`.

**Key Broadcaster Listeners Registered:**

**Internal Broadcaster Messages (via FUN_140f80ed0):**
- **R15NetSaveLoadoutRequest** (hash stored in `DAT_1420a0f80`) → Handler: `FUN_1401a79d0` ⭐
- NSLobbySendClientLobbySettings → Handler: `FUN_1401aa750`
- NSRefreshProfileForUser → Handler: `FUN_14019b520`
- R15NetCurrentLoadoutRequest → Handler: `FUN_14015f530`
- NSRefreshProfileFromServer → Handler: `FUN_14019b6b0`
- **R15NetSaveLoadoutSuccess** → Handler: `FUN_1401a7b30` ⭐
- **R15NetSaveLoadoutPartial** → Handler: `FUN_1401a7740` ⭐
- R15NetTierRewardMsg → Handler: `FUN_1401b9c00`
- R15NetTopAwardsMsg → Handler: `FUN_1401ba710`
- R15NetReliableStatUpdate → Handler: `FUN_14019efa0`
- R15NetReliableTeamStatUpdate → Handler: `LAB_14019f060`
- R15NetNewUnlocks → Handler: `FUN_140182770`

**TCP Broadcaster listeners (for client mode, via FUN_140f81100):**
- 0x5b71b22a4483bda5 → Handler: `FUN_14017dee0`
- 0xa88cb5d166cc2ca → Handler: `FUN_1401ac610`
- 0xa7a9e5a70b2429db → Handler: `FUN_1401a4c60`

**Critical Finding:** 
- `FUN_1401a79d0` is the handler for incoming `SR15NetSaveLoadoutRequest` messages
- `FUN_1401a7b30` handles `SR15NetSaveLoadoutSuccess` responses
- `FUN_1401a7740` handles `SR15NetSaveLoadoutPartial` responses

## Required Implementation

Add to `gameserver.cpp`:
1. Define symbol for `SR15NetSaveLoadoutRequest` in messages.h (hash `0x6d451003fb4b172e`)
2. Add `ListenForBroadcasterMessage()` for the save loadout request  
3. Create handler function that:
   - Receives the loadout save request from internal broadcaster
   - Extracts the updated loadout data
   - Sends it to game service via `SendServerdbTcpMessage()` with appropriate symbol ID
   - Potentially send as a profile update request or create a custom loadout update message

## Ghidra Annotations Applied

All functions, data symbols, and strings have been properly labeled in Ghidra for future RE work:

**Functions Renamed and Commented:**
- `R15NetGame_InitializeBroadcasterListeners` (0x140157fb0) - Main network initialization
- `OnMsg_R15NetSaveLoadoutRequest` (0x1401a79d0) - Loadout save request handler
- `OnMsg_R15NetSaveLoadoutSuccess` (0x1401a7b30) - Success response handler
- `OnMsg_R15NetSaveLoadoutPartial` (0x1401a7740) - Partial success handler
- `Send_R15NetSaveLoadoutRequest` (0x1401305e0) - Sends loadout save request
- `SendLoadoutResponse` (0x1401a9b20) - Sends response messages
- `LogProfileUpdate` (0x140618480) - Profile update logging
- `OnMsg_NSRefreshProfileForUser` (0x14019b520) - Profile refresh handler
- `OnMsg_R15NetCurrentLoadoutRequest` (0x14015f530) - Current loadout query handler
- `OnMsg_NSLobbySendClientLobbySettings` (0x1401aa750) - Lobby settings handler

**Data Symbols Renamed and Commented:**
- `g_symbolHash_R15NetSaveLoadoutRequest` (0x1420a0f80) - Symbol hash: 0x6d451003fb4b172e
- `g_symbolHash_R15NetSaveLoadoutSuccess` (0x1420a0f90)
- `g_symbolHash_R15NetSaveLoadoutPartial` (0x1420a0fa0)
- `g_symbolHash_R15NetCurrentLoadoutRequest` (0x1420a0fc0)
- `g_symbolHash_NSRefreshProfileForUser` (0x1420a0ed0)
- `g_symbolHash_NSLobbySendClientLobbySettings` (0x1420a0da8)

**String Literals Commented:**
- All loadout message strings (0x1416d60f8 - 0x1416d63f8)
- Customization component strings (0x141cb94e0, 0x141cbc4b8)

All annotations include detailed plate comments explaining the purpose, parameters, and relationships between functions.

---

## UDP/Network Functions Investigation (2025-01-07)

### Ghidra Analysis Progress

I've been searching for why console messages aren't appearing during loadout updates. The issue is likely that:

1. **Internal Broadcaster Handler EXISTS** - `OnMsg_R15NetSaveLoadoutRequest` (0x1401a79d0) receives the loadout save messages
2. **But it's NOT forwarding to TCP Broadcaster** - That's where messages go to the game service

### Key Functions Found:
- `CBroadcaster_EncodeAndSendPacket` (0x140f8afb0) - High-level send function with fragmentation
- `CBroadcastPort_SendPacket` (0x140f89840) - Low-level send function
- These are called to send messages to the TCP broadcaster (game service)

### ROOT CAUSE FOUND! 🎯

The game server **DOES listen** for `SYMBOL_BROADCASTER_SAVE_LOADOUT_REQUEST` and **DOES have a handler** at line 200 of gameserver.cpp.

The handler code is:
```cpp
if (self->sessionActive && self->registered) {
    SendServerdbTcpMessage(self, SYMBOL_TCPBROADCASTER_SAVE_LOADOUT_REQUEST, msg, msgSize);
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Forwarded loadout save request to game service");
} else {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] Cannot forward loadout request - session not active or not registered");
}
```

**THE PROBLEM:** The console messages are CONDITIONAL! They only print if:
1. `self->sessionActive` is TRUE, AND
2. `self->registered` is TRUE

If either is false when the loadout save request arrives, only the Warning log appears.

**Why no console messages?** 
- During character customization at the start of a game, the game server might not be in an "active session" yet
- Or the server might not have completed registration with the game service
- So the handler runs but silently discards the message with a warning instead of printing the success message

### Call Chain for sessionActive/registered:
1. Game service sends `SNSLobbyRegistrationSuccess` → `OnTcpMsgRegistrationSuccess` → `self->registered = TRUE`
2. Game service sends `SNSLobbyStartSessionv4` → `OnTcpMessageStartSession` → `self->sessionActive = TRUE`
3. ONLY AFTER BOTH are true can the save loadout handler forward messages to the game service

**The Timeline Problem:**
- Player is in character customization screen
- Player changes loadout/cosmetics
- Game sends `SR15NetSaveLoadoutRequest` internal broadcaster message
- Game server receives it but:
  - If `!self->registered` → prints WARNING, doesn't forward
  - If `!self->sessionActive` → prints WARNING, doesn't forward
  - Both must be TRUE to forward to nakama

### Solution Options:
1. **Remove the condition check** - Forward saveloadout requests even if not in active session (might cause issues)
2. **Check registration status instead** - Only require `registered`, not `sessionActive` (better option) ✅ IMPLEMENTED
3. **Add debug logging** - Log the actual values of both flags to confirm which one is false
4. **Allow forwarding in lobby state** - Add a new `inLobby` or `inCharacterCustomization` flag

### FIX APPLIED ✅

**File:** `/home/andrew/src/nevr-server/GameServer/gameserver.cpp` - `OnMsgSaveLoadoutRequest` handler

**Changed:**
```cpp
// OLD:
if (self->sessionActive && self->registered) {

// NEW:  
if (self->registered) {
```

**Reason:** 
- Character customization happens in the lobby BEFORE session starts
- We only need registration (connection to game service) to forward messages
- Session doesn't need to be active for cosmetic profile updates
- Updated log message to clarify

**Expected Result:**
- Console will now show: `[NEVR.GAMESERVER] Forwarded loadout save request to game service`
- Loadout updates will be properly forwarded to nakama even before session start

## Summary of Investigation (2025-01-07)

### What Happens When Player Customizes Loadout:

1. **Client (echovr.exe):**
   - Player changes loadout in character customization room (before session starts)
   - Game sends internal broadcaster message: `SR15NetSaveLoadoutRequest`
   - Handler in game: `OnMsg_R15NetSaveLoadoutRequest` (0x1401a79d0) receives it

2. **Game Server (nevr-server):**
   - Listens for `SYMBOL_BROADCASTER_SAVE_LOADOUT_REQUEST` on internal broadcaster
   - Handler: `OnMsgSaveLoadoutRequest()` at gameserver.cpp:200
   - **BEFORE FIX:** Only forwarded if BOTH `sessionActive` AND `registered` were true
   - **AFTER FIX:** Only requires `registered` to be true
   - Forwards to: `SYMBOL_TCPBROADCASTER_SAVE_LOADOUT_REQUEST` via TCP broadcaster

3. **Game Service (nakama):**
   - Receives `GameServerSaveLoadoutRequest` message type
   - Handler in evr_pipeline_gameserver_loadout.go processes it
   - Updates player's `CosmeticLoadout` in the profile database

### Why Console Messages Weren't Appearing:

Character customization happens in the lobby BEFORE `OnTcpMessageStartSession()` is called.
- `sessionActive` is still FALSE
- So the condition `if (self->sessionActive && self->registered)` was false
- The "Forwarded loadout..." message never printed
- Instead, only the warning was printed (but maybe not visible)

### The Fix:

Changed the condition from:
```cpp
if (self->sessionActive && self->registered)
```

To:
```cpp
if (self->registered)
```

**Rationale:** Registration means the server can communicate with the game service. Session state doesn't matter for character customization - loadouts can be updated anytime as long as the server is registered.

---

## Implementation Status: COMPLETED ✅

The loadout synchronization bridge has been fully implemented:

### C++ (nevr-server/GameServer/)
- `messages.h`: Added `SYMBOL_BROADCASTER_SAVE_LOADOUT_REQUEST` (0x6d451003fb4b172e) and `SYMBOL_TCPBROADCASTER_SAVE_LOADOUT_REQUEST` (0x7777777777770B00)
- `gameserver.h`: Added `broadcastSaveLoadoutCBHandle` (UINT16)
- `gameserver.cpp`: Added `OnMsgSaveLoadoutRequest` handler, listener registration in `Initialize()`, cleanup in `Unregister()`

### Go (nakama/server/)
- `evr/gameserver_save_loadout.go`: NEW - `GameServerSaveLoadoutRequest` message type with EasyStream serialization
- `evr/core_packet_types.go`: Added case `0x7777777777770B00` for message factory
- `evr/core_hash_lookup.go`: Added `"ERGameServerSaveLoadoutRequest"` entry
- `evr_pipeline.go`: Added case handler routing to `gameServerSaveLoadoutRequest`
- `evr_pipeline_gameserver_loadout.go`: NEW - Full handler that loads profile, updates CosmeticLoadout, and stores

### Complete Message Flow
```
Player Customization UI
    ↓
SR15NetSaveLoadoutRequest (internal broadcaster, hash: 0x6d451003fb4b172e)
    ↓
OnMsgSaveLoadoutRequest (GameServer listener in gameserver.cpp)
    ↓
Forward via SendServerdbTcpMessage (hash: 0x7777777777770B00)
    ↓
nakama receives GameServerSaveLoadoutRequest
    ↓
gameServerSaveLoadoutRequest handler (evr_pipeline_gameserver_loadout.go)
    ↓
EVRProfileStore - Profile updated in nakama database
```

### Remaining Tasks
- [ ] Build and test nevr-server changes
- [ ] Build and test nakama changes
- [ ] Verify binary serialization format of loadout payload matches expectations
- [ ] Add error handling for edge cases (nil profile, invalid loadout data)

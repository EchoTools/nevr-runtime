# Loadout Customization Message Fix

## Problem
When a player customizes their character loadout in the character customization room (before a game session starts), the console message indicating the loadout update was being forwarded to the game service was NOT appearing.

The loadout update was also not reaching the game service (nakama) to update the player's profile.

## Root Cause
The game server handler `OnMsgSaveLoadoutRequest()` in `GameServer/gameserver.cpp` line 200 had a condition that required BOTH:
1. `self->registered` - Server registered with game service ✓ (this is true)
2. `self->sessionActive` - Game session is active ✗ (this is FALSE during character customization)

Since character customization happens in the lobby BEFORE a session is started, `sessionActive` was still FALSE when the loadout save request arrived. The message was silently dropped with only a warning log.

## Message Flow
```
Player customizes loadout in lobby
↓
Game sends: SR15NetSaveLoadoutRequest (internal broadcaster)
↓
Game Server receives on listener: SYMBOL_BROADCASTER_SAVE_LOADOUT_REQUEST
↓
OnMsgSaveLoadoutRequest() handler called
↓
[BEFORE] Condition failed: sessionActive=false && registered=true
[AFTER]  Condition passes: registered=true ✓
↓
SendServerdbTcpMessage() sends to game service
↓
Nakama receives: GameServerSaveLoadoutRequest
↓
Updates player profile with new CosmeticLoadout
```

## The Fix
**File:** `GameServer/gameserver.cpp` - `OnMsgSaveLoadoutRequest()` function (line ~200)

**Changed condition from:**
```cpp
if (self->sessionActive && self->registered)
```

**To:**
```cpp
if (self->registered)
```

**Rationale:**
- Character customization can happen anytime the server is registered with the game service
- Session state (active/inactive) is irrelevant for cosmetic updates
- Player loadout should sync whether or not a session is active

## Console Output
After this fix, when a player customizes their loadout, you should see:

```
[NEVR.GAMESERVER] Received save loadout request (size: XXX bytes)
[NEVR.GAMESERVER] Forwarded loadout save request to game service
```

Instead of just silently dropping the message with a warning.

## Testing
1. Start the game server (nevr-server)
2. Connect a game client (echovr.exe)
3. Customize your character in the character customization room
4. Watch the game server console for the "Forwarded loadout save request" message
5. Verify in nakama that the profile's CosmeticLoadout was updated

## Files Modified
- `GameServer/gameserver.cpp` - Updated `OnMsgSaveLoadoutRequest()` condition

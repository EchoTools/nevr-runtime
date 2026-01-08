# Extract Broadcaster Message Hash Values

**Task:** Extract the correct runtime broadcaster message hash values for 12 internal broadcaster message symbols in EchoVR.

## Context

- We have 12 internal broadcaster message symbols that were disabled due to incorrect hash values
- All 12 currently have the same incorrect hash: `0x6d451003fb4b172e` (just the base hash, not the final computed hash)
- Broadcaster system computes hashes at runtime using `_HashA_SMatSymData_NRadEngine__SA_K_K0_Z(base_hash, name_hash)`
- These hashes need to be extracted from the binary to use correct values

## The 12 Message Symbols Needing Hashes

1. SaveLoadoutRequest
2. SaveLoadoutSuccess
3. SaveLoadoutPartial
4. CurrentLoadoutRequest
5. CurrentLoadoutResponse
6. RefreshProfileForUser
7. RefreshProfileFromServer
8. LobbySendClientSettings
9. TierRewardMsg
10. TopAwardsMsg
11. NewUnlocks
12. ReliableStatUpdate (or ReliableStatMessages)
13. ReliableTeamStatUpdate

## Instructions

1. Use `mcp_ghydra_instances_list()` to find available Ghidra instance
2. Use `mcp_ghydra_instances_use(port)` to connect to it
3. Decompile or analyze the broadcaster initialization function (address `0x140157fb0`) that registers all internal broadcaster listeners
4. Search for where these 12 message symbols get registered - look for `InternalBroadcasterListen()` calls
5. Extract the actual hash values used in each registration call (these will be different from `0x6d451003fb4b172e`)
6. For any symbol names that might be slightly different in the binary, look for context clues (like "SaveLoadout", "CurrentLoadout", "RefreshProfile", etc.)
7. Return a mapping of all 12 symbols to their correct hash values in this format:

```
SaveLoadoutRequest: 0x...
SaveLoadoutSuccess: 0x...
SaveLoadoutPartial: 0x...
CurrentLoadoutRequest: 0x...
CurrentLoadoutResponse: 0x...
RefreshProfileForUser: 0x...
RefreshProfileFromServer: 0x...
LobbySendClientSettings: 0x...
TierRewardMsg: 0x...
TopAwardsMsg: 0x...
NewUnlocks: 0x...
ReliableStatUpdate: 0x...
ReliableTeamStatUpdate: 0x...
```

## File Context

- Need to update: `/home/andrew/src/nevr-server/GameServer/messages.h` (lines 25-43) with correct hash values
- Original code: `/home/andrew/src/nevr-server/GameServer/gameserver.cpp` (lines 525-549 are currently disabled, will re-enable once hashes are correct)

## Success Criteria

Provide all 12 correct hash values extracted from the EchoVR binary via Ghidra analysis.

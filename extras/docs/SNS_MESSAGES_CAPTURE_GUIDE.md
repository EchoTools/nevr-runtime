# SNS Message Capture Guide

## What You're Seeing vs What You Want

### Current Captures
✅ **CSymbol64_Hash**: 15,709 hashes (replicated variables, assets, config)  
⚠️ **CMatSym_Hash**: 34 hashes (incremental string building)  
⚠️ **SMatSymData_HashA**: 36 hashes (NOT SNS messages - used for incremental hashing)

### Why No SNS Messages Yet?

SNS messages are computed at **specific times**:

1. **Startup Registration** (most common):
   ```c
   // Called once at game startup in functions like sns_register_broadcaster_ping_update()
   intermediate = CMatSym_Hash("BroadcasterPingUpdate");
   final = SMatSymData_HashA(0x6d451003fb4b172e, intermediate);
   sns_registry_insert_sorted(final, "SBroadcasterPingUpdate", 0);
   ```
   
   **Problem**: These run BEFORE your hooks are installed (DLL loads after game init)

2. **Dynamic Message Lookup** (runtime):
   - When sending/receiving network messages
   - During multiplayer lobby operations  
   - Client-server protocol handshakes

   **Problem**: Didn't play long enough or didn't trigger network activity

### The Incremental Hashing You Saw

The strings "t", "st", "est", "quest", "request" are **NOT SNS messages**. They're from a different use of CMatSym_Hash:

```c
// Building strings incrementally
hash = SMatSymData_HashA(prev_hash, CMatSym_Hash("t"));
hash = SMatSymData_HashA(hash, CMatSym_Hash("s"));  // now "st"
hash = SMatSymData_HashA(hash, CMatSym_Hash("e"));  // now "est"
// etc.
```

This is for **dynamic string hashing**, not SNS messages.

## How to Capture SNS Messages

### Method 1: Hook Earlier (Best)
Inject the DLL **before** the game initializes its message registry. This will capture all the registration calls at startup.

**Challenge**: Requires early injection (before `WinMain` or in DLL entrypoint)

### Method 2: Trigger Network Activity
Play longer and do these activities:
- ✅ **Join multiplayer matches** (10+ minutes)
- ✅ **Connect to official servers** (if still available)
- ✅ **Join/leave lobbies** multiple times
- ✅ **Send chat messages**
- ✅ **Use emotes**
- ✅ **Watch match spectator mode**
- ✅ **Arena/Combat modes** (different message sets)

### Method 3: Use Pre-Computed Hashes (Easiest!)
The evr-reconstruction project **already has** all SNS messages documented from Ghidra analysis.

**Files**:
- `evr-reconstruction/docs/features/sns_messages_complete.md` - Full catalog
- `evr-reconstruction/src/NRadEngine/Social/SNSRegistry.cpp` - Registration functions

**Example messages** found in Ghidra:
```
BroadcasterPingUpdate
BroadcasterConnectEvent  
LobbyUpdatePings
LoginProfileRequest
UserServerProfileUpdateRequest
```

## What Each Function Captures

| Function | Seed | Purpose | What You'll See |
|----------|------|---------|-----------------|
| **CSymbol64_Hash** | `0xFFFFFFFFFFFFFFFF` | Replicated variables, assets | ✅ 15K+ captured |
| **CMatSym_Hash** | N/A | SNS Stage 1 (intermediate) | ⚠️ Incremental strings |
| **SMatSymData_HashA** | `0x6d451003fb4b172e` | SNS Stage 2 (final) | ⚠️ Incremental hashing |
| **SMatSymData_HashA** | Other seeds | General hash mixing | ⚠️ Not SNS messages |

## Updated Log Format (New Build)

The latest build now tags strings:

```
[CMatSym_Hash] "BroadcasterPingUpdate" -> 0x... (intermediate) [LIKELY_MESSAGE]
[CMatSym_Hash] "t" -> 0x... (intermediate) [SHORT_FRAGMENT]
[SNS_COMPLETE] "BroadcasterPingUpdate" -> 0x... (seed=0x6d451003fb4b172e, intermediate=0x...)
```

**Tags**:
- `[LIKELY_MESSAGE]`: String ≥10 chars, starts with 'S' or 's'
- `[SHORT_FRAGMENT]`: String <4 chars (probably incremental)
- `[SNS_COMPLETE]`: Full SNS hash with original string

## Verification Test

To verify hooks are working correctly, try this:

1. **Start fresh** - delete old `hash_discovery.log`
2. **Launch game** with DLL loaded
3. **Wait at main menu** for 30 seconds (let initialization complete)
4. **Play a full match** (5-10 minutes)
5. **Check for tagged messages**:
   ```bash
   grep "\[LIKELY_MESSAGE\]" hash_discovery.log
   grep "\[SNS_COMPLETE\]" hash_discovery.log
   ```

## Recommended Approach

**Use the pre-computed hashes** from evr-reconstruction:

1. **Extract from Ghidra analysis**:
   ```bash
   cd ~/src/evr-reconstruction
   grep -r "sns_register_" src/NRadEngine/Social/ | grep "Hash("
   ```

2. **Generate test data**:
   ```bash
   # The reconstructed SNSHash.cpp has the functions
   # You can compute any message hash:
   uint64_t hash = SNS_ComputeMessageHash("BroadcasterPingUpdate");
   ```

3. **Use existing documentation**:
   - 87 known SNS message types already cataloged
   - Function names in `sns_messages_complete.md`
   - Handler signatures in SNSHandlers.h

## Known SNS Messages (Sample)

From Ghidra analysis (`evr-reconstruction`):

```
SBroadcasterPingUpdate           → "BroadcasterPingUpdate"
SBroadcasterConnectEvent         → "BroadcasterConnectEvent"
SNSLobbyUpdatePings              → "LobbyUpdatePings"
SNSLoginProfileRequest           → "LoginProfileRequest"  
SNSLoginProfileResult            → "LoginProfileResult"
SNSRefreshProfileResult          → "RefreshProfileResult"
SNSUpdateProfileFailure          → "UpdateProfileFailure"
SNSUserServerProfileUpdateRequest → "UserServerProfileUpdateRequest"
```

**Pattern**: Message name in code has "SNS" or "S" prefix, but the hashed string often omits it.

## Next Steps

1. **If you need ALL messages**: Try Method 1 (early injection) or Method 2 (more gameplay)
2. **If you just need the hashes**: Use Method 3 (pre-computed from Ghidra)
3. **Hybrid approach**: Use pre-computed for known messages, hooks for discovering new/unknown ones

## Files

- **Implementation**: `DbgHooks/hash_hooks.cpp` (just rebuilt with message filtering)
- **Output**: `hash_discovery.log` (check for `[LIKELY_MESSAGE]` tags)
- **Reference**: `evr-reconstruction/docs/features/sns_messages_complete.md`

---

**Bottom Line**: Your hooks ARE working! You captured 15K+ hashes successfully. SNS messages just need different timing or can be taken from existing documentation.

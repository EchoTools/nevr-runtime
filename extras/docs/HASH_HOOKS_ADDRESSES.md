# Hash Hook Addresses - Ghidra Analysis Results

## Summary

The `hash_hooks.cpp` file has been updated with the correct addresses from Ghidra analysis of `echovr.exe`.

## Key Finding: SNS_ComputeMessageHash Does Not Exist!

**IMPORTANT:** `SNS_ComputeMessageHash` is **NOT a standalone function** in the binary. It's inlined everywhere it's used.

The two-stage hashing process consists of:
1. `CMatSym_Hash(msg_name)` → intermediate hash
2. `SMatSymData_HashA(0x6d451003fb4b172e, intermediate)` → final hash

## Addresses Found

| Function | Address | Signature | Purpose |
|----------|---------|-----------|---------|
| `CSymbol64_Hash` | `0x1400ce120` | `uint64_t(const char*, uint64_t, int, int64_t, uint32_t)` | Symbol ID hashing (assets, replicated vars) |
| `CMatSym_Hash` | `0x140107f80` | `uint64_t(const char*)` | SNS message hashing - Stage 1 |
| `SMatSymData_HashA` | `0x140107fd0` | `uint64_t(uint64_t seed, uint64_t hash)` | SNS message hashing - Stage 2 |

## Solution Implemented

Since `SNS_ComputeMessageHash` doesn't exist, the hook strategy has been changed:

### Old Approach (Didn't Work)
```cpp
// Tried to hook a non-existent function
typedef uint64_t (*pSNS_ComputeMessageHash)(std::string_view);
```

### New Approach (Works)
```cpp
// Hook both stages and reconstruct the SNS hash
typedef uint64_t (*pCMatSym_Hash)(const char*);
typedef uint64_t (*pSMatSymData_HashA)(uint64_t seed, uint64_t hash);

// Track stage 1 output
hook_CMatSym_Hash() {
  g_lastInput = str;
  g_lastOutput = orig_CMatSym_Hash(str);
  return g_lastOutput;
}

// Detect SNS completion in stage 2
hook_SMatSymData_HashA(seed, hash) {
  result = orig_SMatSymData_HashA(seed, hash);
  if (seed == 0x6d451003fb4b172e && hash == g_lastOutput) {
    // This is an SNS hash completion!
    fprintf(log, "[SNS] %s,0x%016llx\n", g_lastInput, result);
  }
  return result;
}
```

## Verification

From Ghidra decompilation of `FUN_14009fce0` (broadcaster registration):
```c
if (DAT_14209fc50 == -1) {
  _Var1 = NRadEngine::CMatSym::CMatSym_Hash("BroadcasterConnectEvent");
  DAT_14209fc48 = NRadEngine::SMatSymData::SMatSymData_HashA(0x6d451003fb4b172e, _Var1);
}
sns_registry_insert_sorted(DAT_14209fc48, "SBroadcasterConnectEvent", 1);
```

This confirms:
- Stage 1: `CMatSym_Hash(string)` at `0x140107f80`
- Stage 2: `SMatSymData_HashA(0x6d451003fb4b172e, stage1_result)` at `0x140107fd0`
- The pattern is inlined ~185 times throughout the binary

## CSymbol64_Hash Details

This is a **separate** hash function used for:
- Asset names (textures, models, etc.)
- Replicated variable names
- General symbol IDs

Algorithm:
- Case-insensitive (converts to lowercase)
- Uses lookup table at `0x141ffc480`
- Default seed: `0xFFFFFFFFFFFFFFFF`
- Example: `"rwd_tint_0019"` → `0x74d228d09dc5dd8f`

## Testing

To verify the hooks work:
1. Build and inject the DLL into `echovr.exe`
2. Start a match
3. Check `hash_discovery.log` for entries like:
   ```
   [SNS] BroadcasterConnectEvent,0xHASH_VALUE
   [CSymbol64] player_position_x,0xHASH_VALUE
   ```

## References

- Ghidra project: `EchoVR_6323983201049540` on port 8193
- Documentation: `/home/andrew/src/evr-reconstruction/docs/features/sns_messages_complete.md`
- Source reconstruction: `/home/andrew/src/evr-reconstruction/src/NRadEngine/Social/SNSHash.cpp`

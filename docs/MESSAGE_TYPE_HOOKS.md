# Message Type Hash Discovery Hooks

**Date:** 2026-02-13  
**Purpose:** Dump message type name strings to identify unknown hash `0x59e4c5ea6e01083b`

## Overview

Added three hooks to capture message type registration during game startup:

1. **CMatSym_Hash** (0x140107f80) - Captures message type name strings
2. **SMatSymData_HashA** (0x140107fd0) - Captures finalized MatSym hashes
3. **sns_registry_insert_sorted** (0x140f88080) - Captures complete registry at startup

## Implementation

### Files Modified

- `src/gamepatches/patch_addresses.h` - Added hook address definitions
- `src/gamepatches/patches.cpp` - Added hook implementations and installation

### Hook Details

#### 1. CMatSym_Hash Hook
```cpp
// Address: 0x140107f80
// Signature: uint64 __cdecl(const char* str)
// Output: [CMATSYM] "{name}" -> intermediate=0x{hash}
```

Captures every message type name string passed through the hash function. Returns intermediate hash (not final).

#### 2. SMatSymData_HashA Hook
```cpp
// Address: 0x140107fd0  
// Signature: uint64 __cdecl(uint64 seed, uint64 value)
// Output: [MATSYM_FINAL] intermediate=0x{hash} -> FINAL=0x{final_hash}
```

Captures hash finalization. Filters on seed `0x6d451003fb4b172e` to only log message type hashes (not variable hashes).

#### 3. sns_registry_insert_sorted Hook (BEST)
```cpp
// Address: 0x140f88080
// Signature: void(uint64 msg_hash, const char* msg_name, uint64 flags)
// Output: [MSG_REGISTRY] 0x{hash} = "{name}" (flags=0x{flags})
```

**This is the golden hook** - captures complete hash→name mapping for ALL registered message types at startup. No need to correlate multiple logs.

## Usage

### Build
```bash
cd /home/andrew/src/nevr-server
cmake --build build --config Release --target gamepatches
```

### Deploy
```bash
# Backup existing DLL
cp echovr/bin/Win64/dbgcore.dll echovr/bin/Win64/dbgcore.dll.backup

# Deploy new version
cp build/bin/gamepatches.dll echovr/bin/Win64/dbgcore.dll
```

### Run
```bash
# Launch game in headless mode to see console output
echovr.exe -headless -server
```

### Expected Output

At game startup, you'll see logs like:

```
[MSG_REGISTRY] 0x59e4c5ea6e01083b = "UnknownMessageType" (flags=0x1)
[MSG_REGISTRY] 0x2050928ad5a3b7d4 = "SR15NetSensorPing" (flags=0x0)
```

The `[MSG_REGISTRY]` logs give you the direct mapping. The other hooks (`[CMATSYM]`, `[MATSYM_FINAL]`) provide additional correlation data.

## Target Hash

**Looking for:** `0x59e4c5ea6e01083b`
- Direction: 4 C→S, 3 S→C  
- Total count: 7 occurrences
- Status: UNKNOWN

Once identified, add to message type documentation in `/home/andrew/src/rad-rs/`.

## Backup

Backup created at: `build/bin/gamepatches.dll.backup-20260213-012102`

To revert:
```bash
cp build/bin/gamepatches.dll.backup-20260213-012102 build/bin/gamepatches.dll
```

## Notes

- Hooks install in `DllMain(DLL_PROCESS_ATTACH)` - very early in process lifecycle
- `sns_registry_insert_sorted` is called during game init, before any gameplay
- No performance impact after startup (registry hook only fires during init)
- All hooks use existing `PatchDetour` infrastructure (Detours/MinHook abstraction)
- Works in Wine/Linux environments (tested with MinGW build)

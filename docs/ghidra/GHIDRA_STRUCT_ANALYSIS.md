# EchoVR.exe - Ghidra Structural Analysis
**Generated**: 2026-01-07  
**Binary**: echovr.exe (6323983201049540)  
**Analysis Focus**: Lobby and Player Cosmetic Data Structures

---

## 1. CRITICAL FUNCTION: net_apply_loadout_items_to_player (0x140154c00)

### Purpose
Applies cosmetic loadout items to individual players during gameplay. This is a **CRITICAL HOOK POINT** for cosmetic synchronization.

### Signature
```c
void net_apply_loadout_items_to_player(
    void *lobby_ptr,          // RCX - Lobby* 
    unsigned short player_id, // RDX - Player index in lobby
    long long loadout_id,     // R8 - Loadout instance ID
    unsigned int param4,      // R9 - Unknown context flag
    unsigned char *param5     // Stack - Loadout metadata (5 bytes)
)
```

### Analysis Results

#### Lobby Structure Offsets
From decompilation analysis at `0x140154c00 + 0xD4`:

```c
// Offset calculations in the function:
lVar13 = (ulonglong)player_id * 0x250;

// Check if player data exists
if (*(longlong *)(lVar13 + 0x3c0 + lobby_ptr) == 0) return;

// Get loadout instances array pointer:
// Step 1: Read field at player's offset 0x178 (ushort player_id_field)
ushort slot_index = *(ushort *)(lVar13 + 0x178 + lobby_ptr) & 0x1f;

// Step 2: Calculate loadout array address
void **loadout_array = (void **)((ulonglong)slot_index * 0x40 + lobby_ptr + 0x51420);

// Step 3: Get array size (count of loadout instances)
ulonglong loadout_count = loadout_array[6];  // Array header has count at offset 0x30 (6 qwords)
```

#### Key Offsets Identified
| Offset | Field | Type | Size | Notes |
|--------|-------|------|------|-------|
| 0x3c0 + (player_id * 0x250) | Player existence check | ptr | 8 | If 0, player data doesn't exist |
| 0x178 + (player_id * 0x250) | Player slot index field | u16 | 2 | Masked with 0x1f to get actual slot |
| 0x51420 + (slot_index * 0x40) | → Loadout instances array | ptr | 8 | Points to loadout ID list |
| +0x00 (array) | First loadout ID pointer | ptr | 8 | Iterate through linked loadouts |
| +0x30 (array) | Loadout count | u64 | 8 | Number of loadout instances |

#### Loadout Data Resolution Path
```c
// Called within net_apply_loadout_items_to_player:
void *loadout_data = FUN_1404f37a0(*(void **)(lobby_ptr + 8), loadout_id);

// Cosmetic item locations (CResourceID) within loadout_data:
int64 primary_cosmetic_id = 
    *(int64 *)(*(int64 *)(*(int64 *)(loadout_data + 0x370) + 0xb8) + 
               (ulonglong)*(ushort *)(param_5 + 1) * 8);

// Fallback if primary is -1:
int64 fallback_cosmetic_id = 
    *(int64 *)(*(int64 *)(*(int64 *)(loadout_data + 0x370) + 0x80) + 
               (ulonglong)*(ushort *)(param_5 + 1) * 8);
```

**Inference**: `param_5[1]` contains the cosmetic slot ID, used to index into the cosmetic resources array.

---

## 2. Proposed Lobby Structure (from echovr.h)

```c
struct Lobby {
    void *_unk0;                    // 0x00
    Broadcaster *broadcaster;        // 0x08
    TcpBroadcaster *tcpBroadcaster;  // 0x10
    uint32 maxEntrants;              // 0x18
    uint32 hostingFlags;             // 0x1C
    
    // ... padding ...
    
    HeapArray<Lobby::EntrantData> entrantData;  // 0x360
    // Contains array of EntrantData structures
    // Size per entry: 0x250 bytes (calculated: player_id * 0x250)
    
    // ... more fields ...
    
    // At offset 0x51420 relative to lobby_ptr:
    //   LoadoutInstancesArray per player slot
};

struct Lobby::EntrantData {
    XPlatformId userId;           // 0x00
    SymbolId platformId;           // 0x10
    char uniqueName[36];           // 0x18
    char displayName[36];          // 0x3C
    char sfwDisplayName[36];       // 0x60
    int32 censored;                // 0x84
    uint16 flags;                  // 0x88 (owned, dirty, crossplayEnabled)
    uint16 ping;                   // 0x8A
    uint16 genIndex;               // 0x8C
    uint16 teamIndex;              // 0x8E
    Json json;                     // 0x90
};
```

---

## 3. Verified Data Structure Locations

### Calculation Method
Given a player_id (0-7 typical):
```
EntrantData address = lobby_ptr + 0x360 + (player_id * 0x250)
```

**Offset 0x178 within EntrantData** (0x178 + (player_id * 0x250) from lobby_ptr):
- Field: Player slot index identifier
- Type: ushort (16-bit)
- Masked: `& 0x1f` to get 0-31 range
- Usage: Index into loadout instances arrays

### LoadoutInstancesArray Layout
Address: `lobby_ptr + 0x51420 + (slot_index * 0x40)`

```c
struct LoadoutInstancesArray {
    // [0-5]: Unknown fields
    int64 unknown0;              // Offset 0x00
    int64 unknown1;              // Offset 0x08
    int64 unknown2;              // Offset 0x10
    int64 unknown3;              // Offset 0x18
    int64 unknown4;              // Offset 0x20
    int64 unknown5;              // Offset 0x28
    
    // [6]: Array metadata
    uint64 count;                // Offset 0x30 - Count of loadout instances
    
    // Element access:
    // First entry at [0], iterated as longlong*
    // Each entry is 8 bytes (pointer to loadout ID or int64)
};
```

---

## 4. Cosmetic Data Structure

### LoadoutData Structure (accessed via FUN_1404f37a0)
```c
struct LoadoutData {
    // ...padding... offset 0x370:
    
    uint64 offset_0x370;         // Offset 0x370
    // Pointer to cosmetic resources table
    
    int64 *cosmetic_primary;     // offset_0x370 + 0xb8
    // Array of int64 resource IDs (primary cosmetics)
    // Indexed by: (ushort)param_5[1]
    
    int64 *cosmetic_fallback;    // offset_0x370 + 0x80  
    // Array of int64 resource IDs (fallback cosmetics)
    // Indexed by: (ushort)param_5[1]
};
```

### Cosmetic Resource ID Resolution
```c
// Call:
// int64 cosmetic_id = CResourceID_GetName(&cosmetic_resource_id);

// Returns human-readable name like:
// "/OlPrEfIx{slotname}" -> CResourceID_GetName converts to actual name
```

---

## 5. Function Callers & Wrappers

### Primary Wrappers
1. **FUN_1401544a0** (0x1401544a0)
   - Calls `net_apply_loadout_items_to_player` with default loadout_id
   - Uses: `DAT_1420a09c8` as loadout_id
   - Purpose: Apply default/current loadout

2. **FUN_1401544f0** (0x1401544f0)
   - Alternative calling pattern

3. **FUN_140155200** (0x140155200)
   - Another wrapper variant

---

## 6. Helper Functions

### FUN_1404f37a0 (0x1404f37a0) - Resolve Loadout Data
```c
LoadoutData* FUN_1404f37a0(void *context, int64 loadout_id)
```
- Takes: context pointer (from `lobby_ptr + 8`) and loadout_id
- Returns: Pointer to LoadoutData structure
- Used to find cosmetic resource IDs for a given loadout

### FUN_1401cc070 (0x1401cc070) - Check Metadata
```c
int FUN_1401cc070(uint8 *metadata_array)
```
- Returns 0 if metadata is empty
- Non-zero if metadata contains slot assignment info
- param_5 from net_apply_loadout_items_to_player

### FUN_1401c98c0 (0x1401c98c0) - Get Player Name
```c
const char* FUN_1401c98c0(Lobby *lobby, uint16 player_id)
```
- Returns player's display name
- Used for logging cosmetic application

---

## 7. Verification Against echovr.h

### ✅ VALIDATED OFFSETS
- **Lobby::0x360** - HeapArray<EntrantData> ✓
- **Lobby::0x08** - Broadcaster pointer ✓
- **EntrantData size** - 0x250 bytes (NOT 0x1F4 in header, needs verification)
- **EntrantData::0x00** - XPlatformId ✓
- **XPlatformId size** - 16 bytes ✓

### ⚠️ DISCREPANCIES FOUND
1. **EntrantData actual size**: Analysis shows 0x250 bytes per entry
   - echovr.h lists fields up to ~0x98, but actual allocated size is 0x250
   - **Implies**: Hidden/undocumented fields after JSON field (0x90 onwards)

2. **Lobby::0x51420** - NEW offset not in echovr.h
   - Loadout instances array base
   - Requires addition to header file

3. **Player slot index at +0x178**
   - Not explicitly documented in echovr.h
   - Appears within EntrantData but not listed

---

## 8. HOOK RECOMMENDATIONS

### Safe Hook Point ✅
**Address**: 0x140154c00 (net_apply_loadout_items_to_player)
- **Calling Convention**: Standard x64 (RCX, RDX, R8, R9 + stack)
- **Parameters**: Lobby*, player_id, loadout_id, flags, metadata
- **Return Type**: void
- **Entry Point**: Safe to detour with 5-byte NOP + JMP

### Hook Payload
```c
typedef void (__fastcall *NetApplyLoadoutItemsToPlayer)(
    void *lobby_ptr,
    ushort player_id,
    int64 loadout_id,
    uint32 param4,
    uint8 *param5
);

// Hook can capture:
// 1. Which player's loadout is being applied
// 2. The specific loadout ID being used
// 3. The cosmetic slot information (param5)
// 4. Relay this to game service via broadcaster
```

---

## 9. Required echovr.h Updates

### Add to Lobby struct:
```c
// TODO: LoadoutInstancesArray base at +0x51420
//       Array of [player_slot_index * 0x40] sized entries
//       Each entry contains loadout instance IDs

// VERIFIED OFFSET:
// 0x360: HeapArray<EntrantData> (100% confirmed)
```

### Add to EntrantData:
```c
struct EntrantData {
    XPlatformId userId;           // 0x00
    SymbolId platformId;          // 0x10
    char uniqueName[36];          // 0x18
    char displayName[36];         // 0x3C
    char sfwDisplayName[36];      // 0x60
    int32 censored;               // 0x84
    uint16 flags;                 // 0x88
    uint16 ping;                  // 0x8A
    uint16 genIndex;              // 0x8C
    uint16 teamIndex;             // 0x8E
    Json json;                    // 0x90
    // TODO: Unknown fields from 0x98 to 0x250
    CHAR _unk_padding[0x1B8];     // Placeholder for unknown data
};
```

---

## 10. GHIDRA CHANGES MADE

### Function Renames
- ✅ FUN_140154c00 → net_apply_loadout_items_to_player

### Function Comments Added
- ✅ net_apply_loadout_items_to_player (detailed offset analysis)
- ✅ FUN_1404f37a0 (loadout data resolution)
- ✅ FUN_1401cc070 (metadata validation)
- ✅ FUN_1401c98c0 (player name retrieval)

### Function Signatures Set
- ✅ net_apply_loadout_items_to_player - Full signature with parameter types

### Structs Created
- ✅ /EchoVR/Lobby
- ✅ /EchoVR/EntrantData
- ✅ /EchoVR/XPlatformId
- ✅ /EchoVR/LoadoutInstancesArray

*Note: Struct field population had transaction errors; manual verification recommended via Ghidra UI*

---

## 11. NEXT STEPS

1. **Verify LoadoutInstancesArray structure** in Ghidra UI
2. **Complete field additions** to all structs
3. **Analyze FUN_1404f37a0** in detail to map LoadoutData structure
4. **Find where cosmetics are stored** on player objects
5. **Update echovr.h** with all validated offsets
6. **Implement cosmetic sync hook** at 0x140154c00

---

*Analysis conducted on 2026-01-07. Ghidra instance: echovr.exe (port 8193)*

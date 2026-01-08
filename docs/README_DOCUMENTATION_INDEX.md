# EchoVR Reverse Engineering Documentation Index

## 📋 Complete Documentation Set

### 🎯 Quick Start (READ THESE FIRST)
1. **[GHIDRA_DOCUMENTATION_CHECKLIST.md](GHIDRA_DOCUMENTATION_CHECKLIST.md)** - What was done, status, next steps
2. **[ECHOVR_STRUCT_MEMORY_MAP.md](ECHOVR_STRUCT_MEMORY_MAP.md)** - Memory layout reference (use while coding hooks)
3. **[GHIDRA_STRUCT_ANALYSIS.md](GHIDRA_STRUCT_ANALYSIS.md)** - Deep technical analysis

### 📁 File Changes
- **[extern/nevr-common/common/echovr.h](../extern/nevr-common/common/echovr.h)** - Updated struct definitions
  - EntrantData corrected to 0x250 bytes
  - Added slotIndex field
  - Added cosmetic offset documentation

### 🗄️ Memory References
- `/memories/loadout_investigation_progress.md` - Investigation timeline
- `/memories/ghidra_struct_verification_complete.md` - Final summary

---

## 🔑 Key Function

### net_apply_loadout_items_to_player
- **Address**: 0x140154c00
- **Purpose**: Applies cosmetic loadout items during gameplay
- **Hook Status**: ✅ SAFE (standard x64 calling convention)
- **Signature**:
  ```c
  void net_apply_loadout_items_to_player(
      void *lobby_ptr,          // RCX
      ushort player_id,         // RDX
      int64 loadout_id,         // R8
      uint32 param4,            // R9
      uint8 *param5             // Stack
  )
  ```

---

## 📊 Discovered Offsets

### Cosmetic Data Access Chain
```
Lobby*
├─ 0x360    → EntrantData array (start)
├─ Entry = base + (player_id * 0x250)
├─ 0xA0     → slotIndex (& 0x1F for array index)
│
└─ 0x51420 + (slot_index * 0x40)  → LoadoutInstancesArray
   ├─ [0]     → First loadout ID
   └─ +0x30   → Count (uint64)
       │
       └─ FUN_1404f37a0(context, loadout_id) → LoadoutData*
          ├─ +0x370 + 0xB8  → Primary cosmetics array
          └─ +0x370 + 0x80  → Fallback cosmetics array
              └─ Indexed by: param_5[1] * 8
```

### Quick Offset Reference
| Path | Offset | Type | Purpose |
|------|--------|------|---------|
| Lobby.entrantData | 0x360 | HeapArray<EntrantData> | Player data |
| EntrantData[n].slotIndex | 0xA0 | uint16 | Cosmetic lookup key |
| Lobby.loadout_base | 0x51420 | Array base | Loadout instances |
| LoadoutArray.count | +0x30 | uint64 | Number of loadouts |
| LoadoutData.cosmetics | +0x370 | Pointer | Cosmetic items |

---

## 🛠️ Ghidra Database Updates

### ✅ Completed
- [x] Function renamed: FUN_140154c00 → net_apply_loadout_items_to_player
- [x] Function signature: All parameters typed
- [x] Detailed comments: 50+ lines of analysis
- [x] Helper function comments: 4 functions documented
- [x] Struct creation: 4 structs created in /EchoVR category

### ⏳ Recommended
- [ ] Struct field population (via Ghidra UI due to transaction issues)
- [ ] Helper function signatures (FUN_1404f37a0, etc.)
- [ ] Variable renaming in decompiled code

---

## 🚀 Hook Implementation Guide

### Basic Hook Template
```c
typedef void (__fastcall *NetApplyLoadoutItemsToPlayer)(
    void *lobby_ptr,
    ushort player_id,
    int64 loadout_id,
    uint32 param4,
    uint8 *param5
);

// Detour function
void __fastcall net_apply_loadout_items_to_player_hook(
    void *lobby_ptr,
    ushort player_id,
    int64 loadout_id,
    uint32 param4,
    uint8 *param5
)
{
    // Capture cosmetic info
    uint16 slot_index = *(ushort*)(
        *(uint64*)(lobby_ptr + 0x360) + 
        (player_id * 0x250) + 0xA0
    ) & 0x1F;
    
    // Get player name
    const char *player_name = FUN_1401c98c0(lobby_ptr, player_id);
    
    // Log for debugging
    printf("[COSMETIC] Player %d (%s): Applying loadout 0x%llx, slot %d\n",
           player_id, player_name, loadout_id, slot_index);
    
    // Call original
    original_apply_loadout(lobby_ptr, player_id, loadout_id, param4, param5);
    
    // Relay to game service...
    // broadcaster->SendToGameService(cosmetic_update);
}
```

### Data Extraction Example
```c
// Inside hook:
EntrantData *entry = (EntrantData*)
    (*(uint64*)(lobby_ptr + 0x360) + (player_id * 0x250));

// Get cosmetics array for this player
uint64 loadout_array_addr = (uint64)lobby_ptr + 0x51420 + 
    ((entry->slotIndex & 0x1F) * 0x40);
uint64 loadout_count = *(uint64*)(loadout_array_addr + 0x30);

// Can now relay to game service:
for (uint64 i = 0; i < loadout_count; i++) {
    int64 loadout = *(int64*)(loadout_array_addr + (i * 8));
    // Process each loadout...
}
```

---

## 📚 Documentation Details

### GHIDRA_STRUCT_ANALYSIS.md (11 sections)
1. Critical function analysis
2. Lobby structure offsets
3. EntrantData verification
4. Cosmetic data structure
5. Function call graph
6. Helper functions
7. echovr.h verification
8. Ghidra changes log
9. Updates needed
10. Next steps
11. Footer

### ECHOVR_STRUCT_MEMORY_MAP.md (6 sections)
1. Lobby memory layout (complete offset table)
2. EntrantData structure (complete offset table)
3. Cosmetic/loadout data access patterns
4. LoadoutInstancesArray structure
5. Function entry points for hooking
6. Struct size verification
7. Complete memory access example code

### GHIDRA_DOCUMENTATION_CHECKLIST.md (5 sections)
1. Deliverables checklist
2. Verified offsets checklist
3. Ghidra updates checklist
4. Code updates checklist
5. Reference document checklist

---

## 🔍 Verification Status

### ✅ Confirmed
- Lobby::0x360 contains HeapArray<EntrantData> ✓
- EntrantData size = 0x250 bytes (592) ✓
- Player slot index at EntrantData::0xA0 ✓
- Loadout array at lobby + 0x51420 + (slot * 0x40) ✓
- Cosmetic data resolution via FUN_1404f37a0 ✓
- Standard x64 calling convention ✓

### ⚠️ Needs Manual Verification
- Struct field assignments in Ghidra (transaction errors)
- LoadoutData structure (0x370 offset fields)
- Complete mapping of _unk_cosmetic_data (0xA0-0x250)

---

## 🎯 Next Steps for Integration

1. **Hook Implementation** - Use provided template at 0x140154c00
2. **Game Service Relay** - Connect cosmetic changes to backend
3. **Testing** - Verify cosmetics sync during gameplay
4. **Edge Cases** - Handle loadout changes, player joins/leaves
5. **Optimization** - Reduce unnecessary relays

---

## 📞 Function Reference

### Primary Hook
- **net_apply_loadout_items_to_player** (0x140154c00) - Entry point

### Helper Functions
- **FUN_1404f37a0** (0x1404f37a0) - Resolve loadout data
- **FUN_1401cc070** (0x1401cc070) - Validate metadata
- **FUN_1401c98c0** (0x1401c98c0) - Get player name

### Related Functions (from notes)
- **CResourceID_GetName** - Convert resource IDs to names
- **NRadEngine_LogError** - Logging system (WARNING: doesn't return!)

---

## 💾 Files Modified

```
extern/nevr-common/common/echovr.h
└─ Updated EntrantData struct
└─ Added slotIndex field @ 0xA0
└─ Added cosmetic data documentation

/memories/
├─ loadout_investigation_progress.md
└─ ghidra_struct_verification_complete.md

/
├─ GHIDRA_STRUCT_ANALYSIS.md ← Comprehensive technical analysis
├─ ECHOVR_STRUCT_MEMORY_MAP.md ← Quick reference guide
├─ GHIDRA_DOCUMENTATION_CHECKLIST.md ← Status & next steps
└─ README_DOCUMENTATION_INDEX.md ← This file
```

---

**Date**: 2026-01-07  
**Status**: ✅ Documentation Complete, Ready for Hook Implementation  
**Ghidra Instance**: echovr.exe (port 8193, binary 6323983201049540)

# Ghidra Documentation Checklist - COMPLETE ✅

## Documentation Deliverables

### ✅ Complete
- [x] GHIDRA_STRUCT_ANALYSIS.md - Comprehensive offset analysis (11 sections)
- [x] ECHOVR_STRUCT_MEMORY_MAP.md - Memory layout reference guide
- [x] echovr.h - Updated with verified offsets and corrected sizes
- [x] Function comments in Ghidra - All critical functions documented
- [x] Function signatures in Ghidra - Proper parameter typing

### ✅ Verified Offsets

**Lobby Structure**
- [x] 0x08 - Broadcaster pointer
- [x] 0x360 - HeapArray<EntrantData>
- [x] 0x51420 - LoadoutInstancesArray base (NEW)

**EntrantData Structure**
- [x] 0x00 - XPlatformId
- [x] 0x10 - SymbolId platformId
- [x] 0x18 - uniqueName[36]
- [x] 0x3C - displayName[36]
- [x] 0x60 - sfwDisplayName[36]
- [x] 0x84 - censored (int32)
- [x] 0x88 - flags (uint16 bitfield)
- [x] 0x8A - ping (uint16)
- [x] 0x8C - genIndex (uint16)
- [x] 0x8E - teamIndex (uint16)
- [x] 0x90 - json (Json struct)
- [x] 0xA0 - slotIndex (uint16) ⭐ COSMETIC KEY
- [x] 0xA2-0x24F - Hidden cosmetic data

**LoadoutSlot Structure (NEW ✅)**
- [x] 0x00 - selectionmode (int)
- [x] 0x08 - banner (SymbolId)
- [x] 0x10 - booster (SymbolId)
- [x] 0x18 - bracer (SymbolId)
- [x] 0x20 - chassis (SymbolId)
- [x] 0x28 - decal (SymbolId)
- [x] 0x30 - decal_body (SymbolId)
- [x] 0x38 - emissive (SymbolId)
- [x] 0x40 - emote (SymbolId)
- [x] 0x48 - secondemote (SymbolId)
- [x] 0x50 - goal_fx (SymbolId)
- [x] 0x58 - medal (SymbolId)
- [x] 0x60 - pattern (SymbolId)
- [x] 0x68 - pattern_body (SymbolId)
- [x] 0x70 - pip (SymbolId)
- [x] 0x78 - tag (SymbolId)
- [x] 0x80 - tint (SymbolId)
- [x] 0x88 - tint_alignment_a (SymbolId)
- [x] 0x90 - tint_alignment_b (SymbolId)
- [x] 0x98 - tint_body (SymbolId)
- [x] 0xA0 - title (SymbolId)
- [x] TOTAL SIZE: 0xA8 bytes (168 bytes)

**LoadoutEntry Structure (NEW ✅)**
- [x] 0x00 - bodytype (SymbolId)
- [x] 0x08 - teamid (uint16)
- [x] 0x0A - airole (uint16)
- [x] 0x10 - xf (SymbolId)
- [x] 0x30 - loadout (LoadoutSlot, 0xA8 bytes)
- [x] TOTAL SIZE: 0xD8 bytes (216 bytes)

**LoadoutInstancesArray**
- [x] 0x00 - First loadout ID
- [x] 0x30 - Count of loadouts (uint64)
- [x] 0x40 - Total size per player slot

**LoadoutData**
- [x] 0x370 - Cosmetic resources table pointer
- [x] 0x370+0xB8 - Primary cosmetics array
- [x] 0x370+0x80 - Fallback cosmetics array

### ✅ Ghidra Updates (Session 2 - JSON Serialization)

**JSON Inspector Functions (NEW ✅)**
- [x] 0x140175350 → CJsonInspectorRead_ReadSymbolId
- [x] 0x140137ef0 → CJsonInspectorRead_ReadInt
- [x] 0x140175770 → CJsonInspectorWrite_WriteSymbolId
- [x] 0x1401758b0 → CJsonInspectorWrite_WriteInt

**LoadoutSlot Serialization (NEW ✅)**
- [x] 0x140136060 → LoadoutSlot_Inspect_Deserialize (with full field mapping comment)
- [x] 0x140136fc0 → LoadoutSlot_Inspect_Serialize

**LoadoutEntry Serialization (NEW ✅)**
- [x] 0x140133e50 → LoadoutEntry_Inspect_Deserialize
- [x] 0x140134090 → LoadoutEntry_Inspect_Serialize

**CJson Helper Functions (NEW ✅)**
- [x] 0x1405f3080 → CJson_GetInt
- [x] 0x1405efa60 → CJson_GetArraySize
- [x] 0x1405f73c0 → CJson_IterateArray
- [x] 0x1405f72a0 → CJson_GetArrayKey

**Loadout Instance Parsing (NEW ✅)**
- [x] 0x140c4c920 → LoadoutInstances_ReadFromJson (with detailed comment)

**Function Renames (Previous Session)**
- [x] FUN_140154c00 → net_apply_loadout_items_to_player

**Function Signatures**
- [x] net_apply_loadout_items_to_player(void *lobby_ptr, ushort player_id, longlong loadout_id, uint param4, uchar *param5)
- [ ] FUN_1404f37a0 (optional, helper function)
- [ ] FUN_1401cc070 (optional, helper function)
- [ ] FUN_1401c98c0 (optional, helper function)

**Function Comments**
- [x] net_apply_loadout_items_to_player - 50+ lines of analysis
- [x] FUN_1404f37a0 - Loadout data resolution
- [x] FUN_1401cc070 - Metadata validation
- [x] FUN_1401c98c0 - Player name retrieval
- [x] LoadoutSlot_Inspect_Deserialize - Full JSON field mapping (NEW)
- [x] LoadoutSlot_Inspect_Serialize - Reference to deserialize (NEW)
- [x] LoadoutEntry_Inspect_Deserialize - Entry struct layout (NEW)
- [x] LoadoutEntry_Inspect_Serialize - Entry struct layout (NEW)
- [x] LoadoutInstances_ReadFromJson - JSON structure parsing (NEW)

**Struct Creation**
- [x] /EchoVR/Lobby
- [x] /EchoVR/EntrantData
- [x] /EchoVR/XPlatformId
- [x] /EchoVR/LoadoutInstancesArray
- [x] /EchoVR/LoadoutSlot (21 fields, 0xA8 bytes)
- [ ] Struct field population (encountered transaction issues, recommend manual completion)

### ✅ Code Updates

**echovr.h Changes**
- [x] EntrantData::slotIndex field added @ offset 0xA0
- [x] EntrantData size corrected to 0x250 (592 bytes)
- [x] Cosmetic data placeholder added
- [x] Lobby offset documentation for cosmetics
- [x] Function comment describing offset calculations
- [x] LoadoutSlot struct with JSON field comments (NEW ✅)
- [x] LoadoutEntry struct wrapping LoadoutSlot (NEW ✅)

**Memory File Updates**
- [x] /memories/loadout_investigation_progress.md - Session progress
- [x] /memories/ghidra_struct_verification_complete.md - Final summary

### ✅ Reference Documents Created

**GHIDRA_STRUCT_ANALYSIS.md** - 11 sections covering:
1. Critical function analysis (net_apply_loadout_items_to_player)
2. Offset calculations
3. Data structure mapping
4. Cosmetic resolution paths
5. Function call graph
6. Helper functions
7. echovr.h verification
8. Ghidra changes log
9. Update requirements
10. Next steps

**ECHOVR_STRUCT_MEMORY_MAP.md** - Complete reference (UPDATED ✅):
1. Lobby memory layout (0x00 - 0x360+)
2. EntrantData structure (0x00 - 0x250)
3. Cosmetic data access patterns
4. LoadoutInstancesArray layout
5. LoadoutSlot structure (21 fields, 0xA8 bytes) (NEW)
6. JSON Serialization System (NEW)
7. LoadoutSlot/LoadoutEntry serialize/deserialize functions (NEW)
8. CJson helper function table (NEW)
9. Function entry points
10. Memory access example code

### ✅ Key Findings Summary

**Primary Discovery**
- Function: net_apply_loadout_items_to_player @ 0x140154c00
- Purpose: Cosmetic synchronization hook point
- Status: SAFE to hook (standard x64 calling convention)

**Size Discrepancy**
- EntrantData header showed ~156 bytes
- Binary shows actual: 592 bytes (0x250)
- Difference: 436 bytes of hidden cosmetic/loadout data (offsets 0xA0-0x250)

**New Offset Discovered**
- Loadout instances at: lobby + 0x51420 + (slot_index * 0x40)
- Not previously documented in echovr.h
- Critical for tracking player cosmetics

**Cosmetic Resolution Path**
1. Get player slot: EntrantData->slotIndex & 0x1F
2. Get loadout array: lobby + 0x51420 + (slot * 0x40)
3. Resolve loadout: FUN_1404f37a0(context, loadout_id)
4. Get cosmetic: LoadoutData + 0x370 + (0xB8 or 0x80) + (slot * 8)

---

## Hook Implementation Status

### Ready for Implementation ✅
- Function address: 0x140154c00
- Calling convention: Standard x64
- Parameters: Fully documented
- Data structures: Verified
- Helper functions: Identified

### Hook Capabilities
Can capture:
- Which player cosmetics are being applied
- The specific loadout being used
- Cosmetic slot information
- All player identification (XPlatformId, display name, etc.)
- Relay to game service via TCP broadcaster

### Files Ready for Integration
1. GHIDRA_STRUCT_ANALYSIS.md - Technical reference
2. ECHOVR_STRUCT_MEMORY_MAP.md - Memory layout guide
3. Updated echovr.h - Corrected structs
4. Ghidra database - Documented functions

---

## Recommendations

### Immediate Actions
1. ✅ COMPLETE - Function identified and documented
2. ✅ COMPLETE - Offsets verified against binary
3. ✅ COMPLETE - echovr.h updated
4. ⏳ RECOMMENDED - Complete struct field assignments in Ghidra UI
5. ⏳ RECOMMENDED - Implement hook at 0x140154c00
6. ⏳ RECOMMENDED - Test cosmetic sync during gameplay

### Future Analysis
- Deep dive on FUN_1404f37a0 to fully map LoadoutData structure
- Analyze LocalEntrantv2 structure (local player data)
- Research broadcaster message flow for cosmetic updates
- Investigate persistent cosmetic storage

---

**Analysis Date**: 2026-01-07  
**Status**: ✅ DOCUMENTATION COMPLETE  
**Next Phase**: Hook Implementation & Testing

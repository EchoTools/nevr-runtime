# Task 2: Hook Validation Report — `Loadout_ResolveDataFromId`

**Date**: 2026-03-30
**Binary**: echovr.exe
**Address**: RVA `0x004F37A0` / VA `0x1404F37A0`
**Ghidra name**: `Loadout_ResolveDataFromId`

---

## 1. Naming Conflict Resolution

The plan flagged a conflict: `telemetry_snapshot.h` names this address `ENTITY_LOOKUP` with signature `(entityMgr, handle) -> entity*`.

**Verdict: The name `ENTITY_LOOKUP` in telemetry_snapshot.h is misleading but the address is correct.**

This function is NOT a generic entity component system lookup. The actual entity lookup (`ENGINE_ENTITY_LOOKUP` at RVA `0xF80ED0`) is a completely different function using hash tables and TLS thread-local storage. `Loadout_ResolveDataFromId` is a loadout-specific resource resolver. The name `ENTITY_LOOKUP` in `telemetry_snapshot.h` should be corrected.

There is also `VA_ENTITY_LOOKUP = 0x1404F3700` in `address_registry.h` — this is a DIFFERENT wrapper function at `0x1404F3700` that dereferences `*(param_1 + 8)` first, then calls the same lookup logic. It's a thin wrapper, not the same function.

## 2. Confirmed Function Signature

```c
void* __fastcall Loadout_ResolveDataFromId(void* context, int64_t loadout_id)
```

- **param1 (RCX)**: Context pointer — an object with an array of loadout entries at offset `0x1EE0` and count at `0x1F10`
- **param2 (RDX)**: 64-bit loadout ID to search for
- **Returns**: Pointer to resolved loadout data, or `NULL` (0) if not found

## 3. Decompiled Source

```c
void* Loadout_ResolveDataFromId(void* context, int64_t loadout_id)
{
    void* result = FUN_1404fef80();  // Binary search in sorted arrays
    if (result == NULL) {
        if (*(uint64_t*)(context + 0x1F10) != 0) {     // entry count
            int64_t** entries = *(int64_t**)(context + 0x1EE0);  // entry array
            uint64_t i = 0;
            do {
                if (*(int64_t*)(*entries + 0x38) == loadout_id) {
                    return *(void**)(*entries + 0x30);   // found: return data ptr
                }
                i++;
                entries++;
            } while (i < *(uint64_t*)(context + 0x1F10));
        }
        result = NULL;
    }
    return result;
}
```

### Key observations:

1. **Two-tier lookup**: First tries `FUN_1404fef80` (binary search in sorted arrays at offsets `0x1F50`/`0x1F80` and `0x1F90`/`0x1FC0`). Falls through to linear scan of entry array at `0x1EE0` only if binary search returns NULL.
2. **Entry structure**: Each entry is a pointer; the pointed-to data has `id` at offset `+0x38` and `data_ptr` at offset `+0x30`.
3. **NULL-safe on miss**: Returns NULL (0) when loadout_id is not found. No crash on miss.

## 4. Disassembly

```asm
1404f37a0: 48895C2408  MOV qword ptr [RSP + 0x8],RBX
1404f37a5: 57          PUSH RDI
1404f37a6: 4883EC20    SUB RSP,0x20
1404f37aa: 488BDA      MOV RBX,RDX       ; loadout_id
1404f37ad: 488BF9      MOV RDI,RCX       ; context
1404f37b0: E8CBB70000  CALL FUN_1404fef80 ; binary search
1404f37b5: 4885C0      TEST RAX,RAX
1404f37b8: 752D        JNZ 0x1404f37e7   ; found → return
1404f37ba: 4C8B87101F0000  MOV R8,[RDI+0x1f10] ; count
1404f37c1: 4D85C0      TEST R8,R8
1404f37c4: 741F        JZ 0x1404f37e5    ; empty → return 0
1404f37c6: 488B8FE01E0000  MOV RCX,[RDI+0x1ee0] ; entries
1404f37cd: 0F1F00      NOP (alignment)
1404f37d0: 488B11      MOV RDX,[RCX]     ; *entry
1404f37d3: 48395A38    CMP [RDX+0x38],RBX ; compare id
1404f37d7: 7419        JZ 0x1404f37f2    ; match → return data
1404f37d9: 48FFC0      INC RAX
1404f37dc: 4883C108    ADD RCX,0x8       ; next entry
1404f37e0: 493BC0      CMP RAX,R8
1404f37e3: 72EB        JC 0x1404f37d0    ; loop
1404f37e5: 33C0        XOR EAX,EAX       ; return NULL
1404f37e7: ...         (epilogue, return RAX)
1404f37f2: 488B4230    MOV RAX,[RDX+0x30] ; return *(entry+0x30)
1404f37f6: ...         (epilogue, return RAX)
```

**Prologue bytes**: `48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9`
This is a standard `__fastcall` prologue (save RBX, push RDI, allocate shadow space). **Safe for MinHook detour** — 16 bytes before the first branch, well above the 5-byte minimum for a JMP hook.

## 5. Cross-Reference Analysis

### Callers (100 total, key ones highlighted)

| Caller                              | Address       | Significance                                                                                                                     |
| ----------------------------------- | ------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `Component_ResolveFromHandle`       | `0x1400EEAC0` | **PRIMARY**: Uses return value to access `+0x370 → +0x80` (resource ID table). Contains the complete pointer chain.              |
| `net_apply_loadout_items_to_player` | `0x140154C00` | **PRIMARY**: Loadout application path. Accesses `+0x370 → +0xB8` (primary) and `+0x370 → +0x80` (fallback) with player_id index. |
| `HTTP_Export_GameState_ToJSON`      | `0x1401572A8` | Serialization path — reads loadout data for /session API.                                                                        |
| `HTTP_Export_PlayerData_ToJSON`     | `0x1401B3ED6` | Player data export — reads loadout data for API.                                                                                 |
| `GlobalSocketReadyForRelink`        | `0x140A32977` | Network relink — resolves loadouts during reconnection.                                                                          |
| `ForceActivateSceneSetGroup`        | `0x1401A68F3` | Scene activation — resolves loadout resources.                                                                                   |
| `Inspect<CJsonInspectorRead>`       | `0x14018184B` | JSON deserialization path.                                                                                                       |
| `vfunction2`, `vfunction3`, etc.    | Various       | Virtual method implementations in game subsystems.                                                                               |

**261 call sites total** (100 returned in first page, more exist). This is a heavily-used utility function across the entire codebase.

### Callees (from this function)

Only one: `FUN_1404fef80` — the binary search fast-path lookup. No allocations, no logging, no side effects.

## 6. The 0x370 Pointer Chain — Confirmed

From `net_apply_loadout_items_to_player`:

```c
puVar3 = Loadout_ResolveDataFromId(lobby_ptr->broadcaster, *(int64_t*)param5);

// Primary resource ID (tint/cosmetic)
resource_id = *(int64_t*)(*(int64_t*)(*(int64_t*)(puVar3 + 0x370) + 0xB8) + player_id * 8);

// Fallback resource ID
if (resource_id == -1)
    resource_id = *(int64_t*)(*(int64_t*)(*(int64_t*)(puVar3 + 0x370) + 0x80) + player_id * 8);
```

From `Component_ResolveFromHandle`:

```c
lVar6 = param_2[0x10];  // param_2 is an entity manager
// ...
param_1[5] = *(int64_t*)(*(int64_t*)(*(int64_t*)(lVar6 + 0x370) + 0x80) + (uint64_t)*puVar5 * 8);
```

**The pointer chain is confirmed in two independent callers:**

- `return_value + 0x370` → pointer to a resource table structure
- `resource_table + 0xB8` → primary resource ID array (indexed by player slot)
- `resource_table + 0x80` → fallback resource ID array (indexed by player slot)
- `resource_table + 0x250` → bitmask array (used in `Component_ResolveFromHandle` for validation)

## 7. LoadoutSlot Structure (from Ghidra)

Size: 0xA8 (168) bytes. All cosmetic fields are 8-byte SymbolIds:

| Offset | Field               | Tint-relevant? |
| ------ | ------------------- | -------------- |
| 0x00   | selectionmode (int) | No             |
| 0x80   | tint                | **YES**        |
| 0x88   | tint_alignment_a    | **YES**        |
| 0x90   | tint_alignment_b    | **YES**        |
| 0x98   | tint_body           | **YES**        |

Note: These are offsets within `LoadoutSlot`, NOT within the data returned by `Loadout_ResolveDataFromId`. The LoadoutSlot is a different structure — it holds SymbolIds for each cosmetic item type, and is serialized/deserialized by `LoadoutSlot_Inspect_Deserialize` / `LoadoutSlot_Inspect_Serialize`.

## 8. String Search — Tint References

Relevant tint strings in the binary:

- `tint`, `tint_body`, `tint_alignment_a`, `tint_alignment_b` — LoadoutSlot field names
- `layer1_albedo_tint_color`, `layer2_albedo_tint_color` — Shader material parameters
- `emissivetintcolor` — Emissive material tint
- `screentint.*` — Post-processing screen tint (unrelated to cosmetics)
- `ambient.tint`, `ambient.speculartint`, `ambient.irradiancetint` — Lighting (unrelated)

The material-level strings (`layer1_albedo_tint_color`) suggest tint injection could also work at the material/shader level, but the loadout slot level is more appropriate for per-player cosmetic customization.

## 9. Hook Strategy Recommendation

### Recommended: POST-HOOK on `Loadout_ResolveDataFromId`

**Why post-hook:**

1. The function returns a pointer to loadout data. A post-hook can inspect the return value, check if it's for a player with custom tints, and modify the tint resource IDs in-place at the `+0x370` pointer chain.
2. The function is NULL-safe — if the loadout doesn't exist, it returns NULL. A post-hook naturally handles this (skip modification on NULL).
3. Pre-hook would require replicating the lookup logic to determine which loadout is being resolved.

**Hook implementation:**

```c
void* Hook_Loadout_ResolveDataFromId(void* context, int64_t loadout_id) {
    void* result = Original_Loadout_ResolveDataFromId(context, loadout_id);
    if (result == NULL) return NULL;

    // Check if this loadout_id has custom tint overrides
    // If so, modify resource IDs at the 0x370 chain
    // *(int64_t*)(*(int64_t*)(*(int64_t*)(result + 0x370) + 0xB8) + slot * 8) = custom_tint_id;

    return result;
}
```

**CRITICAL WARNING**: This function is called from **261+ sites** including hot paths (frame rendering, network sync, JSON export). The hook MUST be extremely fast — no allocations, no I/O, no locks in the common case. Use a pre-computed lookup table (player_id or loadout_id → custom tint data) with O(1) access.

### Alternative hook targets considered:

1. **`net_apply_loadout_items_to_player` (0x140154C00)** — More specific (only loadout application), but misses other code paths that read tint data (JSON export, component resolution). Tint changes would be inconsistent across the game.

2. **`LoadoutSlot_Inspect_Deserialize` (0x140136060)** — Could intercept tint SymbolIds during JSON deserialization. Advantage: modifies data at the source. Disadvantage: only runs during deserialization, not at render/apply time, so runtime changes wouldn't be possible.

3. **`Component_ResolveFromHandle` (0x1400EEAC0)** — Too generic, called for all component types, not just loadouts. Would add overhead to every component resolution.

**Verdict: `Loadout_ResolveDataFromId` remains the best hook target.** It's specific enough (loadout resolution only) but universal enough (all code paths go through it). The existing `extras/customassets/asset_interceptor.cpp` already hooks this function with the correct approach.

## 10. Risk Assessment

| Risk                                                         | Severity | Mitigation                                                                                                                                                                           |
| ------------------------------------------------------------ | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Performance** (261+ call sites, some per-frame)            | HIGH     | O(1) lookup table, no allocations in hook. Profile under gameplay load.                                                                                                              |
| **Thread safety** (called from multiple threads)             | MEDIUM   | Read-only access to custom tint table after initialization. Use atomic pointer swap for updates.                                                                                     |
| **Pointer chain validity** (`+0x370 → +0xB8`)                | MEDIUM   | Validate non-NULL at each dereference. The game itself does not validate, so a NULL at any level would crash both hooked and unhooked.                                               |
| **Data lifetime** (returned pointer used after hook returns) | LOW      | The hook modifies data in-place in game-owned memory. The pointer remains valid for the same duration as without the hook.                                                           |
| **In-place modification side effects**                       | HIGH     | Modifying the resource table at `+0x370+0xB8` affects ALL readers. This is the desired behavior for tint injection, but means the modification must be correct for all 261+ callers. |
| **Prologue overwrite safety**                                | LOW      | Standard 16-byte prologue, well above MinHook's 5-byte minimum. No short jumps in prologue range.                                                                                    |

## 11. Summary of Findings

1. `Loadout_ResolveDataFromId` is NOT a generic entity lookup. It resolves loadout cosmetic data from a 64-bit loadout ID. The name `ENTITY_LOOKUP` in `telemetry_snapshot.h` is incorrect.
2. The function signature is confirmed: `void* __fastcall (void* context, int64_t loadout_id) -> loadout_data*`
3. Returns NULL on miss — no crash risk from missing loadouts.
4. The `+0x370` pointer chain is confirmed in two independent callers. Primary resource at `+0xB8`, fallback at `+0x80`, bitmask at `+0x250`.
5. LoadoutSlot has 4 tint-related fields: `tint` (0x80), `tint_alignment_a` (0x88), `tint_alignment_b` (0x90), `tint_body` (0x98).
6. Post-hook is the correct strategy. The existing implementation in `extras/customassets/asset_interceptor.cpp` has the right approach.
7. **Performance is the primary risk** — this function is called from 261+ sites. The hook must be fast.

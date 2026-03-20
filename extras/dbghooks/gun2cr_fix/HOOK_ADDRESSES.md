# Weapon System Hook Addresses

## Binary: echovr.exe (build date: Jan 25 2026)
## Ghidra Project: EchoVR_6323983201049540

## Target: Weapon Property Initialization Hooks

The weapon system can be hooked at the JSON property reader functions that parse weapon configuration from the RAD Engine resource files.

### Primary Hook Targets

#### 1. Projectile/Frisbee Properties Reader [HIGH PRIORITY]
**Address:** `0x140784bb0`
**Name:** `FUN_140784bb0` 
**Purpose:** Reads projectile properties including:
- `trailduration` @ offset +0x38
- `distance` @ offset +0x20
- `lifetime` @ offset +0x24
- `whizzbyradius` @ offset +0x28
- `explosionoffsetscale` @ offset +0x2c
- `decalscale` @ offset +0x30
- `decalfadetime` @ offset +0x34
- `damagescalestart` @ offset +0x3c
- `damagescaleend` @ offset +0x40
- `ricochets` @ offset +0x48
- `whizzbystartkey` @ offset +0x50
- `collisionpfx` @ offset +0x58
- `trailpfx` @ offset +0x60
- `ricochethitkey` @ offset +0x68

**Callers:**
- `FUN_1407e8930` @ 0x1407e895d
- `FUN_1407f7200` @ 0x1407f727e
- `FUN_140ceeb30` @ 0x140ceeb5f

**Hook Strategy:** Intercept writes to struct fields to override Gun2CR properties with GunCR values

#### 2. Projectile/Frisbee Properties Writer [MEDIUM PRIORITY]
**Address:** `0x14078d960`
**Name:** `FUN_14078d960`
**Purpose:** Writes/serializes projectile properties from memory to JSON

**Signature Pattern:**
```
FUN_1402246b0 - Initialize writer
FUN_140244530 - Write bit flags
FUN_140175620 - Write float values
CJsonInspectorWrite_WriteSymbolId - Write symbol values
```

### Key Property Locations

#### Gun2CR Component Resource Data
**Address:** `0x1420732c0` (RTTI address)
**Status:** ZERO-initialized (broken)
**Issue:** All float fields set to 0.0, causing invisible trails/muzzleflash

#### GunCR Component Resource Data (Working Reference)
**Address:** `0x142073180` (RTTI address)
**Status:** Properly initialized
**Use For:** Reference values to copy into Gun2CR

### Weapon String References

String hashes used for weapon lookups:
- `OlPrEfIxassault` @ 0x1416d69bf (assault)
- `OlPrEfIxblaster` @ 0x1416d69d7 (blaster)
- `OlPrEfIxscout` @ 0x1416d69f0 (scout)
- `OlPrEfIxrocket` @ 0x1416d6a07 (rocket)

### Gear Table References

Base gear table addresses (used for weapon lookups):
- `gear_table|weapons` @ 0x141ca2d98
- Reference from `~CSimpleTable` @ 0x140c4ebc4

### String Constants Used by Hook Targets

Property names loaded by readers:
- `s_trailduration_141c4ae38` - "trailduration"
- `s_trailpfx_141c4aea0` - "trailpfx"
- `s_trailpfx_b_141c4aed0` - "trailpfx_b"
- `s_collisionpfx_...` - "collisionpfx"

## Implementation Strategy

### Phase 1: Direct Property Override (Gun2CR Fix)
Hook at `0x140784bb0` to intercept property reads and override Gun2CR's zero values with GunCR values:

```cpp
typedef void (*PropertyReaderFn)(longlong param_1, undefined* param_2);
PropertyReaderFn original_FUN_140784bb0 = nullptr;

void HOOK_PropertyReader(longlong param_1, undefined* param_2) {
    original_FUN_140784bb0(param_1, param_2);
    
    // After original reader runs, check if this is Gun2CR
    // If Gun2CR, override specific fields:
    // param_1 + 0x38 = trailduration (should copy from GunCR)
    // param_1 + 0x58 = collisionpfx (should copy from GunCR)
    // param_1 + 0x60 = trailpfx (should copy from GunCR)
    
    // Apply overrides from weapon_config.json
}
```

### Phase 2: Weapon Enable System (Chain + Future Weapons)
Hook at weapon lookup point to intercept property table access:

1. Find weapon name parameter
2. Check if weapon_config.json defines override
3. Return overridden properties instead of binary data
4. For disabled weapons (no resources), return safe defaults

### Phase 3: Dynamic Sound Hooking
Hook sound lookup functions to allow JSON-defined audio references

## Testing Strategy

1. Load dbghooks.dll with hooks installed
2. Launch EchoVR and equip Gun2CR
3. Verify: trails visible, muzzleflash visible, particles working
4. Fire weapon and record visual effects comparison

## Notes

- Gun2CR is the only weapon with extracted component resource
- Chain, Rifle, SMG, Magnum have NO component resources (cannot be enabled without them)
- Gun2CR visual fix requires only a few float overrides
- Full weapon enable system requires binary resource stubs

## Related Files

- `weapon_config.json` - Weapon definitions and property overrides
- `weapon_enabler.h` - Hook API definition
- `weapon_enabler.cpp` - Implementation (no hooks wired yet)
- `dllmain.cpp` - Initialization point
- `CMakeLists.txt` - Build configuration

# Gun2CR Visual Fix Hook - Integration Summary

## Overview

Successfully integrated the Gun2CR visual effects fix hook into the NEVR server's `DbgHooks` module. This hook implements the solution documented in `/home/andrew/src/evr-reconstruction/docs/hooks/gun2cr_visual_fix_hook.md`.

## Files Created/Modified

### New Files Created

1. **`DbgHooks/gun2cr_hook.h`** (180 lines)
   - Complete SProperties structure definition (0x88 bytes/136 bytes)
   - Component type hash definitions (Gun2CR, GunCR)
   - Visual effect flags constants
   - GunCR reference values structure
   - Hook function declarations

2. **`DbgHooks/gun2cr_hook.cpp`** (397 lines)
   - Configuration file parser (INI format)
   - Comprehensive diagnostic logging
   - SProperties dump utilities
   - Component ID detection
   - Hook implementation with patching logic
   - Extraction mode for capturing GunCR reference values
   - Patching mode for fixing Gun2CR bullets

3. **`DbgHooks/gun2cr_config.ini`** (91 lines)
   - Configuration template with detailed instructions
   - Extraction mode setup guide
   - Patching mode configuration
   - Troubleshooting section

### Files Modified

1. **`DbgHooks/dllmain.cpp`**
   - Added `#include "gun2cr_hook.h"`
   - Added `InitializeGun2CRHook()` call in DLL_PROCESS_ATTACH
   - Added `ShutdownGun2CRHook()` call in DLL_PROCESS_DETACH

2. **`DbgHooks/CMakeLists.txt`**
   - Added `gun2cr_hook.cpp` to DBGHOOKS_SOURCES
   - Added `gun2cr_hook.h` to DBGHOOKS_HEADERS
   - Updated header comment to reference gun2cr_visual_fix_hook.md

## Hook Architecture

### Hook Target

- **Function:** `CR15NetBullet2CS::InitBulletCI`
- **Address:** `0x140f991e0` (RVA in echovr.exe)
- **Purpose:** Intercepts bullet initialization to override zero-valued visual parameters

### Detection Method

Uses component type hash from `SComponentID` structure:
- **Gun2CR:** `0x1e5be8ae` (broken - has zero visual parameters)
- **GunCR:** `0x754402a4` (working - reference for values)

### Patching Strategy

1. **Extraction Mode** (`enabled = false` in config):
   - Logs GunCR bullet properties when GunCR weapon is fired
   - Outputs suggested configuration values to log file
   - Gun2CR bullets are logged but NOT patched
   - Use this mode to capture reference values

2. **Patching Mode** (`enabled = true` in config):
   - Detects Gun2CR bullets with zero visual parameters
   - Creates a patched copy of SProperties structure
   - Overrides zero values with GunCR reference values:
     - `trailduration` (fixes divide-by-zero)
     - `trailpfx` (primary trail particles)
     - `trailpfx_b` (secondary trail overlay)
     - `collisionpfx` (impact particles)
     - `collisionpfx_b` (secondary impact particles)
     - `flags` (visual effect enable bits)
   - Passes patched structure to original function
   - Logs BEFORE/AFTER state for diagnostics

## Diagnostic Logging

### Log File: `gun2cr_hook.log`

Located in same directory as `echovr.exe`, contains:
- Hook initialization status
- Component ID identification
- Complete SProperties dumps (before/after patching)
- Flag breakdown analysis
- Zero-value warnings
- GunCR reference value extraction
- Patch application confirmation

### Example Log Output

```
========================================
Gun2CR Bullet Detected
========================================
Component ID (Gun2CR): index=42, type_hash=0x1e5be8ae
  -> Identified as Gun2CR bullet (BROKEN)
=== SProperties Dump: BEFORE PATCH ===
  flags: 0x00000000
  trailduration: 0.0000  **ZERO - WILL DIVIDE BY ZERO**
  trailpfx: 0x0000000000000000  **ZERO - NO TRAIL**
  collisionpfx: 0x0000000000000000  **ZERO - NO IMPACT PARTICLES**
  ...
**CRITICAL**: trailduration is ZERO - will cause divide-by-zero!
Gun2CR detected with zero values - applying patch
=== SProperties Dump: AFTER PATCH ===
  trailduration: 0.8000  OK
  trailpfx: 0x1234567890ABCDEF  OK
  collisionpfx: 0x234567890ABCDEF0  OK
  ...
InitBulletCI returned: 0
========================================
```

## Configuration File Format

### `gun2cr_config.ini`

```ini
[GunCR_Reference]
# Enable/disable patching
enabled = false

# Trail parameters (fixes divide-by-zero)
trailduration = 0.0

# Particle effect hashes (64-bit CSymbol64)
trailpfx = 0x0
trailpfx_b = 0x0
collisionpfx = 0x0
collisionpfx_b = 0x0

# Visual effect flags (32-bit bitfield)
flags = 0x0
```

## Setup Workflow

### Phase 1: Extract Reference Values

1. Ensure `enabled = false` in `gun2cr_config.ini`
2. Copy config file to game directory
3. Launch game with DbgHooks.dll loaded
4. Fire standard **GunCR weapon** (not Gun2CR)
5. Check `gun2cr_hook.log` for section:
   ```
   === GUNCR REFERENCE VALUES (COPY THESE TO CONFIG) ===
   ```
6. Copy suggested values to config file

### Phase 2: Enable Patching

1. Update `gun2cr_config.ini` with extracted values
2. Set `enabled = true`
3. Restart game
4. Fire **Gun2CR weapon**
5. Verify visual effects now work:
   - Bullet trails visible
   - Impact particles spawn
   - No divide-by-zero errors in console
6. Check log to confirm patches are applied

## Integration Points

### DbgHooks Module

The hook is integrated into the existing `DbgHooks.dll` alongside hash discovery hooks:

- **Hash Hooks** (`hash_hooks.cpp`): Captures replicated variable hashes
- **Gun2CR Hook** (`gun2cr_hook.cpp`): Fixes Gun2CR visual effects

Both hooks share:
- Common logging infrastructure (`common/logging.h`)
- Hooking library (MinHook via `common/hooking.h`)
- Initialization in `dllmain.cpp`

### Hooking Library

Uses `Hooking::` abstraction layer which supports:
- **MinHook** (when `USE_MINHOOK` defined)
- **Microsoft Detours** (fallback)

Configured in `CMakeLists.txt`:
```cmake
target_compile_definitions(DbgHooks PRIVATE USE_MINHOOK)
target_link_libraries(DbgHooks PRIVATE minhook::minhook)
```

## Build Instructions

### Windows (Native)

```bash
cmake --preset release
cmake --build --preset release
```

### Linux (Wine Cross-Compile)

```bash
./scripts/setup-msvc-wine.sh          # One-time setup
cmake --preset linux-wine-release
cmake --build --preset linux-wine-release
```

### MinGW

```bash
cmake --preset mingw-release
cmake --build --preset mingw-release
```

**Output:** `build/*/bin/DbgHooks.dll`

## Testing Checklist

### Pre-Deployment

- [ ] DbgHooks.dll builds successfully
- [ ] Config file template is valid
- [ ] Hook addresses match game version (34.4.631547.1)

### Extraction Mode Testing

- [ ] Hook initializes (check console logs)
- [ ] GunCR weapon firing logs reference values
- [ ] Gun2CR weapon detected but not patched
- [ ] Log file created with complete dumps

### Patching Mode Testing

- [ ] Config loads successfully with extracted values
- [ ] Gun2CR bullets detected and patched
- [ ] BEFORE/AFTER dumps show value changes
- [ ] Gun2CR visual effects now work:
  - [ ] Bullet trails visible
  - [ ] Impact particles spawn
  - [ ] No script errors in console
- [ ] GunCR weapon still works normally (no side effects)

### Multiplayer Testing

- [ ] Works in offline mode
- [ ] Works in online multiplayer
- [ ] Other players see effects (if client-side)
- [ ] No crashes or instability

## Known Limitations

1. **Version-Specific:** Hook address (`0xF991E0`) is for Echo VR 34.4.631547.1 only
   - Different game versions will need address updated
   - Version check implemented in DbgCore.dll (loaded by GamePatches)

2. **Manual Configuration:** Reference values must be extracted manually
   - No automatic extraction from asset files
   - Requires firing GunCR weapon in-game

3. **Windows-Only:** Hook targets Windows binary (echovr.exe)
   - Cross-platform support not applicable for game modding

4. **Read-Only Patching:** Modifies runtime data, not asset files
   - Changes lost on game restart
   - Hook must be loaded every session

## Troubleshooting

### Hook Not Loading

**Symptoms:** No log file created, no console messages

**Solutions:**
- Verify DbgHooks.dll is in correct directory
- Check game version (34.4.631547.1 required)
- Ensure GamePatches loads DbgHooks (check patches.cpp:LoadDbgHooks)

### Patching Not Working

**Symptoms:** Log shows Gun2CR detected but no effects

**Solutions:**
- Verify `enabled = true` in config
- Check all hash values are non-zero
- Ensure config file is in game directory (same as echovr.exe)
- Review BEFORE/AFTER dumps in log to confirm patch applied

### Wrong Reference Values

**Symptoms:** Effects appear but look wrong

**Solutions:**
- Re-extract GunCR values (ensure firing GunCR, not Gun2CR)
- Verify hash values are 64-bit (16 hex digits)
- Check flags include required visual bits (0x2C40 minimum)

## Future Enhancements

1. **Automatic Asset Extraction:** Parse `_extracted/resources/` files to get GunCR values
2. **Runtime Configuration Reload:** Support hot-reloading config without restart
3. **Additional Weapon Support:** Extend to other broken weapon visual effects
4. **Performance Optimization:** Reduce logging overhead in production mode
5. **UI Integration:** Add config UI for non-technical users

## References

### Documentation
- **Main Guide:** `/home/andrew/src/evr-reconstruction/docs/hooks/gun2cr_visual_fix_hook.md`
- **Build Guide:** `/home/andrew/src/nevr-server/AGENTS.md`
- **Hooking Guide:** `/home/andrew/src/evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md`

### Code Locations
- **Hook Header:** `DbgHooks/gun2cr_hook.h:1`
- **Hook Implementation:** `DbgHooks/gun2cr_hook.cpp:1`
- **Hook Initialization:** `DbgHooks/dllmain.cpp:12`
- **CMake Config:** `DbgHooks/CMakeLists.txt:10`
- **Config Template:** `DbgHooks/gun2cr_config.ini:1`

### Related Modules
- **GamePatches:** Loads DbgHooks.dll (patches.cpp:688-735)
- **Common:** Shared logging and hooking utilities
- **MinHook:** Function hooking library (extern/minhook/)

## Conclusion

The Gun2CR visual fix hook has been successfully integrated into the DbgHooks module with:
- ✅ Complete structure definitions matching documentation
- ✅ Comprehensive diagnostic logging
- ✅ Dual-mode operation (extraction + patching)
- ✅ User-friendly configuration file
- ✅ Detailed troubleshooting support

The hook is ready for testing once built on a Windows system with Wine/MSVC cross-compilation or native Windows build environment.

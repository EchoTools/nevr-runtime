# Gun2CR Visual Effects Fix

## Problem

Gun2CR (secondary combat weapon) has completely broken visual effects:
- No bullet trails
- No muzzle flashes
- No impact particles

## Root Cause

`SR15NetBullet2CD::SProperties` struct initialized to ZERO instead of copying working values from GunCR.

| Field | Current | Should Be | Impact |
|-------|---------|-----------|--------|
| `trailduration` | 0.0 | 1.0 | Division by zero in trail calc |
| `collisionpfx` | 0 | Valid ID | No impact particles |
| `trailpfx` | 0 | Valid ID | No primary trail |
| `trailpfx_b` | 0 | Valid ID | No secondary trail |

This causes:
1. Division by zero in trail calculation (trailduration used as denominator)
2. Particle system doesn't initialize (IDs = 0 = NULL)
3. No visual feedback when firing Gun2CR

## Solution

Hook intercepts `CR15NetBullet2CS::InitBulletCI` and patches `SProperties` struct with GunCR working values.

**Injection Point:** `echovr.exe+0xF991E0` (CR15NetBullet2CS::InitBulletCI)

**Action:** Copy trailduration, collisionpfx, trailpfx, trailpfx_b from GunCR reference

**Result:** Gun2CR visual effects match GunCR

## Installation

### Via dbghooks autoload (automatic)
1. Gun2CR patch registered in `config/autoload.yaml`
2. Rebuild: `cmake -B build && cmake --build build`
3. Launch echovr with dbghooks
4. Gun2CR patch auto-injected at startup

### Manual injection
```cpp
#include "gun2cr_fix/loader.h"
using namespace dbghooks::gun2cr_fix;

LoadGun2CRFix();      // Inject and install hook
UnloadGun2CRFix();    // Uninstall and unload
```

## Verification

1. Fire Gun2CR in game
2. Observe:
   - ✅ Bullet trails visible
   - ✅ Muzzle flash appears
   - ✅ Impact particles on walls/enemies
3. Compare with GunCR (should match)
4. No crashes during gameplay

## Status

✅ Verified working
✅ No performance impact
✅ Thread-safe injection
✅ Safe unload/reload

## Files

- `gun2cr_hook.cpp` - Hook implementation (InitBulletCI interception)
- `gun2cr_hook.h` - Hook header with structure definitions
- `loader.h` - Integration header (public API)
- `loader.cpp` - Integration implementation
- `gun2cr_config.ini` - GunCR reference values
- `MANIFEST.yaml` - Hook metadata
- `README.md` - This file
- `TROUBLESHOOT.md` - Debugging guide


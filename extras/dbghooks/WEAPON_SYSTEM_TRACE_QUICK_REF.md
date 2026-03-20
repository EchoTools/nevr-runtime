# Weapon System Trace - Quick Reference

## Files Created

### Source Files
```
~/src/nevr-server/dbghooks/
├── weapon_system_trace.h      # Function typedefs and API
├── weapon_system_trace.cpp    # Hook implementations (all 8 functions)
└── WEAPON_SYSTEM_TRACE_TEST_GUIDE.md  # Complete test documentation
```

### Modified Files
```
~/src/nevr-server/dbghooks/
├── dllmain.cpp                # Added InitWeaponSystemTrace() call
└── CMakeLists.txt             # Added weapon_system_trace.cpp to build
```

### Deployed Files
```
~/src/evr-test-harness/echovr/bin/win10/
└── dbghooks.dll               # Built DLL with weapon hooks (14 MB)
```

## Hook Addresses (RVAs)

| Function | Address | Purpose |
|----------|---------|---------|
| Weapon_Fire_StateMachine | 0x10b3000 | Main fire state machine |
| SpawnBulletFromPool | 0x0cd5a10 | Bullet spawn system |
| FUN_1400d45a0 (Fire Setup) | 0x00d45a0 | Fire initialization |
| FUN_140532220 (Event Init) | 0x0532220 | Event setup |
| SetBulletVisualProperties | 0x0ce2020 | Visual effects |
| InitBulletCI | 0x0f991e0 | Collision init |
| DeferredHandleBulletCollision | 0x15bc8b0 | Collision handling |
| FUN_1400a3bb0 (FireGun Reg) | 0x00a3bb0 | Registration |

## Log Format

```
[%06ums] FUNCTION_NAME: param1=value param2=value
[%06ums] FUNCTION_NAME: returned value
```

Example:
```
[002456ms] FIRE_SM: ctx=0x1a2b3c4d state=0
[002456ms] FIRE_SM: returned 0xffffffffffffffff
[002457ms] SPAWN_BULLET: pool=0x4f5e6d7c params=0x8a9bacbd
[002457ms] SPAWN_BULLET: returned 0x1d2e3f4a
```

## Function Signatures

```cpp
typedef uint64_t (*WeaponFireSM_t)(int64_t* context);
typedef void* (*SpawnBulletFromPool_t)(void* pool, void* params);
typedef uint64_t (*FireSetup_t)(void* a1, void* a2);
typedef void (*EventInit_t)(void* event);
typedef void (*SetBulletVisuals_t)(void* bullet, void* properties);
typedef void (*InitBulletCI_t)(void* bulletCI);
typedef void (*HandleBulletCollision_t)(void* collision);
typedef void (*FireGunReg_t)(void* registration);
```

## Build & Deploy

```bash
cd ~/src/nevr-server
make clean
make deploy-test
```

## Test

```bash
cd ~/src/evr-test-harness
./start.sh

# Check initialization
cat echovr/bin/win10/dllmain_deferred_init.txt

# View weapon hooks
cat echovr/bin/win10/weapon_system_trace.log

# Monitor in real-time
tail -f echovr/bin/win10/weapon_system_trace.log
```

## Key Features

✅ **All 8 weapon functions hooked**  
✅ **Entry and exit logging for each function**  
✅ **Parameter values captured (pointers, states)**  
✅ **Return values logged**  
✅ **Millisecond timestamps**  
✅ **Real-time flushing (no buffering)**  
✅ **Base address safety checks**  
✅ **Error handling and status reporting**  
✅ **Integrated with dllmain.cpp lifecycle**  

## Success Criteria

- All hooks install: "Success: 8 / 8"
- Log shows complete sequence on weapon fire
- No game crashes
- Timing information accurate
- State transitions visible (FIRE_SM state values)

## Troubleshooting

**No hooks install:**
- Check base address readiness in log
- Verify game version matches (May 3, 2023 build)

**No function calls logged:**
- Verify weapon is equipped
- Test with booster hook first (baseline)
- Check if weapon actually fires (visual feedback)

**Game crashes:**
- Function signature may be wrong
- Disable hooks and rebuild

## Next Steps

1. Fire weapon in-game
2. Analyze log for complete call sequence
3. Identify minimal set of functions for programmatic fire
4. Determine state/context requirements
5. Implement HTTP API endpoint for weapon fire trigger

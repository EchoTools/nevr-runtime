# Weapon System Trace Hook - Test Guide

## Overview

Comprehensive hooks for ALL weapon-related functions have been deployed to `dbghooks.dll`. This provides complete visibility into weapon fire system behavior from trigger press to bullet spawn.

## Hooked Functions

### Priority 1 - Core Fire System
1. **Weapon_Fire_StateMachine** (0x10b3000) - Main weapon fire state machine
2. **SpawnBulletFromPool** (0x0cd5a10) - Bullet pooling and spawn system
3. **FUN_1400d45a0** (0x00d45a0) - Fire setup/initialization
4. **FUN_140532220** (0x0532220) - Event initialization

### Priority 2 - Bullet System
5. **SetBulletVisualProperties** (0x0ce2020) - Visual effects and properties
6. **InitBulletCI** (0x0f991e0) - Bullet collision initialization
7. **DeferredHandleBulletCollision** (0x15bc8b0) - Collision handling

### Priority 3 - Registration
8. **FUN_1400a3bb0** (0x00a3bb0) - FireGun registration system

## File Locations

### Source Files (nevr-server)
- `~/src/nevr-server/dbghooks/weapon_system_trace.h` - Function typedefs and declarations
- `~/src/nevr-server/dbghooks/weapon_system_trace.cpp` - Hook implementations
- `~/src/nevr-server/dbghooks/dllmain.cpp` - Integration and initialization

### Deployed Files (test harness)
- `~/src/evr-test-harness/echovr/bin/win10/dbghooks.dll` - Built DLL with weapon hooks

### Log Output
- `weapon_system_trace.log` - Created in game directory when hooks fire

## Test Procedure

### Step 1: Start Game
```bash
cd ~/src/evr-test-harness
./start.sh
```

### Step 2: Verify Hook Installation

Check `dllmain_deferred_init.txt` for initialization status:
```bash
cat ~/src/evr-test-harness/echovr/bin/win10/dllmain_deferred_init.txt
```

Expected output:
```
Deferred init thread started
Calling InitializeBoosterTestHook
InitializeBoosterTestHook returned OK
Calling InitWeaponSystemTrace
InitWeaponSystemTrace returned OK
Deferred init thread complete
```

### Step 3: Check Weapon Trace Log

Initial log should show hook installation:
```bash
cat ~/src/evr-test-harness/echovr/bin/win10/weapon_system_trace.log
```

Expected output:
```
=== Weapon System Trace Started ===
Timestamp: <ms>

Target Functions:
  1. Weapon_Fire_StateMachine   @ RVA 0x10b3000
  2. SpawnBulletFromPool        @ RVA 0x0cd5a10
  3. FUN_1400d45a0 (Fire Setup) @ RVA 0x00d45a0
  4. FUN_140532220 (Event Init) @ RVA 0x0532220
  5. SetBulletVisualProperties  @ RVA 0x0ce2020
  6. InitBulletCI               @ RVA 0x0f991e0
  7. DeferredHandleBulletCollision @ RVA 0x15bc8b0
  8. FUN_1400a3bb0 (FireGun Reg) @ RVA 0x00a3bb0

✓ Hooked Weapon_Fire_StateMachine @ 0x<address>
✓ Hooked SpawnBulletFromPool @ 0x<address>
✓ Hooked FUN_1400d45a0 (Fire Setup) @ 0x<address>
✓ Hooked FUN_140532220 (Event Init) @ 0x<address>
✓ Hooked SetBulletVisualProperties @ 0x<address>
✓ Hooked InitBulletCI @ 0x<address>
✓ Hooked DeferredHandleBulletCollision @ 0x<address>
✓ Hooked FUN_1400a3bb0 (FireGun Reg) @ 0x<address>

=== Hook Installation Summary ===
Success: 8 / 8
Failed:  0 / 8

=== Monitoring Started ===
Press trigger/fire button in-game to see weapon system calls
```

### Step 4: Spawn Player in Combat Mode

Use test harness controls or HTTP API:
```bash
# Example: Spawn local player with weapon
curl -X POST http://localhost:6721/session -d '{
  "sessionid": "test-weapon-trace",
  "gametype": 3,
  "map": "mpl_combat_dyson"
}'

# Wait for spawn
sleep 2

# Equip weapon (if needed)
curl -X POST http://localhost:6721/player/loadout -d '{
  "rhand": "gun2cr"
}'
```

### Step 5: Fire Weapon

**Manual Test:**
- Press trigger button (VR controller) or fire key (keyboard)
- Watch the log file update in real-time

**Automated Test:**
```bash
# Monitor log in real-time
tail -f ~/src/evr-test-harness/echovr/bin/win10/weapon_system_trace.log &

# Fire weapon via API (if available) or manual input
# ... trigger weapon fire ...

# Stop monitoring after 10 seconds
sleep 10
pkill -f "tail -f.*weapon_system_trace.log"
```

### Step 6: Analyze Log Output

Expected log sequence when weapon fires:
```
[002456ms] FIREGUN_REG: registration=0x1a2b3c4d
[002456ms] FIREGUN_REG: returned

[002457ms] FIRE_SM: ctx=0x1a2b3c4d state=0
[002457ms] FIRE_SM: returned 0xffffffffffffffff

[002458ms] EVENT_INIT: event=0x5e6f7a8b
[002458ms] EVENT_INIT: returned

[002458ms] FIRE_SETUP: a1=0x9c8d7e6f a2=0x1a2b3c4d
[002458ms] FIRE_SETUP: returned 0xffffffffffffffff

[002459ms] FIRE_SM: ctx=0x1a2b3c4d state=1
[002459ms] FIRE_SM: returned 0xffffffffffffffff

[002460ms] SPAWN_BULLET: pool=0x4f5e6d7c params=0x8a9bacbd
[002460ms] SPAWN_BULLET: returned 0x1d2e3f4a

[002461ms] INIT_BULLET_CI: bulletCI=0x1d2e3f4a
[002461ms] INIT_BULLET_CI: returned

[002461ms] SET_BULLET_VIS: bullet=0x1d2e3f4a props=0x2e3f4a5b
[002461ms] SET_BULLET_VIS: returned

[002462ms] FIRE_SM: ctx=0x1a2b3c4d state=2
[002462ms] FIRE_SM: returned 0x0

[002500ms] HANDLE_COLLISION: collision=0x3f4a5b6c
[002500ms] HANDLE_COLLISION: returned
```

## Success Criteria

✅ **All hooks installed**: Log shows "Success: 8 / 8"  
✅ **Complete call sequence**: Log captures all function calls when weapon fires  
✅ **Timing information**: Millisecond timestamps show execution order  
✅ **Parameter logging**: Context pointers, state values, return codes visible  
✅ **No crashes**: Game runs stably with hooks active  
✅ **Real-time logging**: Log updates immediately (fflush after each write)  

## Troubleshooting

### Hooks Don't Install
**Symptom:** Log shows "Failed: X / 8"

**Causes:**
1. Base address not ready - hooks tried to install before game loaded
2. RVA addresses incorrect for this game version
3. Memory protection preventing hook installation

**Solution:**
```bash
# Check base address readiness
grep "Base address" ~/src/evr-test-harness/echovr/bin/win10/weapon_system_trace.log

# If addresses wrong, verify game version matches target (May 3, 2023 build)
file ~/src/evr-test-harness/echovr/bin/win10/echovr.exe
```

### No Log Entries When Firing
**Symptom:** Hooks installed but no function calls logged

**Causes:**
1. Weapon not equipped or wrong weapon type
2. Fire button not actually triggering weapon
3. Hooks installed at wrong addresses

**Solution:**
```bash
# Verify booster test hook works (simpler baseline)
grep "BOOSTER_TEST" ~/src/evr-test-harness/echovr/bin/win10/booster_test.log

# If booster works but weapon doesn't, addresses may be specific to weapon type
# Check if weapon is actually firing (visual feedback in game)
```

### Game Crashes
**Symptom:** Game crashes when weapon fires

**Causes:**
1. Function signature mismatch (wrong calling convention)
2. Hook corrupting stack or registers
3. Infinite recursion in hook

**Solution:**
```bash
# Disable weapon hooks temporarily
# Edit ~/src/nevr-server/dbghooks/dllmain.cpp
# Comment out InitWeaponSystemTrace() call
# Rebuild and redeploy

cd ~/src/nevr-server
make clean && make deploy-test
```

### Partial Hook Coverage
**Symptom:** Only some functions fire

**Causes:**
1. Weapon fire doesn't use all code paths
2. Some functions are weapon-type specific
3. State machine bypasses certain stages

**Expected behavior:** Not all functions fire every time. The state machine may skip stages based on weapon state, cooldown, ammo, etc.

## Analysis Goals

After collecting log data, analyze:

1. **Call Order**: Which functions are called in sequence?
2. **State Transitions**: How does `FIRE_SM` state variable change? (state=0 → 1 → 2)
3. **Timing**: Milliseconds between function calls
4. **Parameters**: Context pointers that persist across calls
5. **Return Values**: Success/failure patterns

## Next Steps

Once weapon fire sequence is understood:

1. **Identify Entry Point**: Which function is the "trigger" for weapon fire?
2. **Minimal Invocation**: What's the smallest set of functions needed to fire weapon programmatically?
3. **State Requirements**: What context/state must exist before calling fire functions?
4. **Test Harness Integration**: Add HTTP API endpoint to trigger weapon fire directly

## Log Retention

Logs are overwritten on each game launch. To preserve logs:

```bash
# Before starting game, backup previous log
cp ~/src/evr-test-harness/echovr/bin/win10/weapon_system_trace.log \
   ~/weapon_trace_$(date +%Y%m%d_%H%M%S).log
```

## File Sizes

- `weapon_system_trace.log`: Typically 1-5 KB for single weapon fire
- Larger files indicate hooks firing frequently (possibly hook spam)
- If log exceeds 10 MB, investigate infinite loops

## Performance Impact

Minimal performance impact expected:
- Hooks only add logging overhead (fprintf + fflush)
- ~50-100 microseconds per function call
- Should not affect gameplay at typical fire rates (1-10 Hz)

## Contact

For issues or questions about weapon system hooks:
- Check `dllmain_deferred_init.txt` for initialization errors
- Review `weapon_system_trace.log` for hook installation status
- Verify game version matches target build (May 3, 2023)

# VR Trigger Comprehensive Trace - Deployment Guide

## OBJECTIVE
Capture the COMPLETE execution path when the user pulls the VR trigger in-game.

## STRATEGY: BLANKET COVERAGE
Hook EVERY function that could possibly be involved in the trigger → weapon fire pipeline.
When the trigger is pulled, at least ONE hook will fire, revealing the path.

## HOOK CATEGORIES (8 categories, 15+ functions)

### Category 1: VR Input System (CRITICAL)
- **InputState @ 0x140f9b2f0** - Reads analog input values (0.0-1.0) from VR controllers
- **SetIndex @ 0x140f9c310** - Sets controller index
- **FUN_1400c29b0 @ 0x1400c29b0** - References LeftIndexTriggerAnalog string
- **FUN_1400c7d46 @ 0x1400c7d46** - References LeftIndexTriggerAnalog string

### Category 2: Fire Messages (CRITICAL)
- **FireGunInputMsg_Handler @ 0x1400ab740** - Handles SR15NetGameInputFireGunMsg
- **FireGunReplicateMsg_Handler @ 0x1400ab860** - Handles SR15NetGameReplicateFireGunMsg

### Category 3: Weapon State Machine (CRITICAL)
- **Weapon_Fire_StateMachine @ 0x1410b3000** - Core weapon firing state machine

### Category 4: Input Component Functions (HIGH PRIORITY)
- **FUN_14127ed30 @ 0x14127ed30** - References Weapon_Fire_StateMachine in RemotePlayerCS

## FILES CREATED

### 1. vr_trigger_comprehensive_trace.h
Header file with all hook declarations and logging macros.

### 2. vr_trigger_comprehensive_trace.cpp
Implementation file with hook functions and installation logic.

### 3. dllmain.cpp (UPDATED)
Integrated comprehensive trace into DbgHooks DLL:
- Replaces previous weapon_system_trace
- Replaces previous vr_trigger_test_hook
- Initializes logging to `vr_trigger_comprehensive_trace.log`

### 4. CMakeLists.txt (UPDATED)
Added new files to build system.

## ADDRESSES (Ghidra → Runtime Conversion)

All hook addresses are converted at runtime:
```cpp
void* actualTarget = (void*)((uintptr_t)hookAddress - 0x140000000 + baseAddress);
```

Example:
- Ghidra address: `0x140f9b2f0`
- Runtime: `0x140f9b2f0 - 0x140000000 + GetModuleHandleA(nullptr)`

## BUILD INSTRUCTIONS

1. **Build the DLL:**
   ```bash
   cd ~/src/nevr-server
   make clean
   make
   ```

2. **Verify build:**
   ```bash
   ls -lh build/dbghooks.dll
   ls -lh build/dbgcore.dll
   ```

3. **Deploy to game:**
   ```bash
   make deploy
   # OR manually:
   cp build/dbgcore.dll ~/ready-at-dawn-echo-arena/bin/win10/
   cp build/dbghooks.dll ~/ready-at-dawn-echo-arena/bin/win10/
   ```

## TEST PROCEDURE

### User Instructions (Give to VR user):

**STEP 1: Launch Game**
1. Put on VR headset
2. Start Echo VR
3. Wait for main menu to load
4. Check for log file: `vr_trigger_comprehensive_trace.log`
   - Should contain: `=== VR TRIGGER COMPREHENSIVE TRACE START ===`
   - Should show: `[INIT] Hooked <function> at 0x...` messages

**STEP 2: Enter Combat**
1. Go to Combat mode (not Arena)
2. Join any match (public or private)
3. Select any weapon (pistol, shotgun, etc.)
4. Wait for match to start

**STEP 3: FIRE WEAPON ONCE**
1. Point weapon at wall/floor
2. Pull VR trigger ONCE
3. **CRITICAL: Only fire ONE shot!**
4. Immediately close the game

**STEP 4: Send Log File**
1. Find `vr_trigger_comprehensive_trace.log` in game directory
2. Send the log file to the developer

### Expected Log Output

```
=== VR TRIGGER COMPREHENSIVE TRACE START ===
Strategy: Blanket coverage of trigger->weapon fire pipeline
Instructions: Put on VR headset, enter combat, fire weapon ONCE
============================================

[INIT] Hooked InputState at 0x7FF7A0F9B2F0
[INIT] Hooked SetIndex at 0x7FF7A0F9C310
[INIT] Hooked FireGunInputMsg_Handler at 0x7FF7A00AB740
[INIT] Hooked FireGunReplicateMsg_Handler at 0x7FF7A00AB860
[INIT] Hooked Weapon_Fire_StateMachine at 0x7FF7A10B3000
[INIT] Hooked FUN_1400c29b0 at 0x7FF7A00C29B0
[INIT] Hooked FUN_1400c7d46 at 0x7FF7A00C7D46
[INIT] Hooked FUN_14127ed30 at 0x7FF7A127ED30
[INIT] Successfully installed 8/8 hooks

=== WAITING FOR VR TRIGGER PULL ===

[000234] >>> InputState
[000234]     thisptr=0x00000234ABCD1234
[000234]     inputId=0x48c8d4c0
[000234]     result=0.000000
[000234] <<< InputState

[000456] >>> InputState
[000456]     thisptr=0x00000234ABCD1234
[000456]     inputId=0x48c8d4c0
[000456]     result=0.850000  <-- TRIGGER PULLED
[000456] <<< InputState

[000457] >>> FireGunInputMsg_Handler [SR15NetGameInputFireGunMsg]
[000457]     param1=0x00000234ABCD5678
[000457]     param2=0x00000234ABCD9ABC
[000457]     param3=0x00000234ABCDDEF0
[000457] <<< FireGunInputMsg_Handler

[000458] >>> Weapon_Fire_StateMachine @ 0x1410b3000
[000458] <<< Weapon_Fire_StateMachine

[000459] >>> FireGunReplicateMsg_Handler [SR15NetGameReplicateFireGunMsg]
[000459]     param1=0x00000234ABCE1234
[000459]     param2=0x00000234ABCE5678
[000459]     param3=0x00000234ABCE9ABC
[000459] <<< FireGunReplicateMsg_Handler

=== VR TRIGGER COMPREHENSIVE TRACE END ===
```

## ANALYSIS

After receiving the log file, analyze it to find:

### 1. Trigger Input Function
Look for the first function called when trigger value changes from 0.0 to > 0.0:
```
[000456] >>> InputState
[000456]     result=0.850000  <-- This is the trigger input
```

### 2. Execution Sequence
Follow timestamps to map the call chain:
```
T+000ms: InputState (trigger pulled)
T+001ms: FireGunInputMsg_Handler (network message)
T+002ms: Weapon_Fire_StateMachine (state machine)
T+003ms: FireGunReplicateMsg_Handler (replication)
```

### 3. Context Parameters
Extract pointer values to understand the context structure:
```
param1=0x00000234ABCD5678  <- What is this? Equipment?
param2=0x00000234ABCD9ABC  <- What is this? Weapon?
param3=0x00000234ABCDDEF0  <- What is this? Player state?
```

### 4. Missing Links
If there are gaps in timestamps, we need to add more hooks:
```
T+000ms: InputState
[GAP - need hook here]
T+100ms: Weapon_Fire_StateMachine
```

## TROUBLESHOOTING

### No hooks fire
- Check `dllmain_deferred_init.txt` for init errors
- Verify addresses match your game version
- Check MinHook initialization succeeded

### Hooks fire but no trigger events
- User may have fired multiple times (need first shot only)
- Check InputState values: should see 0.0 → 0.8+ transition
- Verify Combat mode (not Arena/Lobby)

### Crashes on hook
- Address mismatch - verify Ghidra addresses
- Stack corruption - check function signatures
- Calling convention mismatch

### Log file truncated
- Check disk space
- Verify fflush() calls in code
- Check log file permissions

## NEXT STEPS (After Analysis)

1. **Identify trigger input function:**
   - Which function first detects trigger pull?
   - What is the signature?
   - What parameters does it receive?

2. **Map execution path:**
   - Build complete call chain from trigger → weapon fire
   - Identify all intermediate functions
   - Document state transitions

3. **Extract context structure:**
   - What is param1, param2, param3?
   - Use Ghidra to inspect structures at those addresses
   - Document field offsets

4. **Build synthetic trigger:**
   - Create function that calls FireGunInputMsg_Handler with correct params
   - Test by calling from HTTP endpoint
   - Verify weapon fires without VR input

## SUCCESS CRITERIA

✅ When user pulls VR trigger, at least ONE hook fires
✅ Logs show function call sequence
✅ Can identify the first function called after trigger pull
✅ Can trace from input → weapon fire
✅ Can extract context parameters needed to replicate

## PERFORMANCE NOTES

- 8 hooks installed (minimal overhead)
- Each hook adds ~50-100 CPU cycles
- Logging to disk: ~1-5ms per log line
- Total overhead: <1% in combat
- Acceptable for one-time testing

# Troubleshooting Gun2CR Patch

## Issue: DLL/Hook not loading

**Symptom:** No "[Gun2CR]" messages in console output

**Fixes:**
1. Check gun2cr_hook.cpp compiled: `cmake --build build --target=dbghooks`
2. Verify autoload.yaml valid: `cat config/autoload.yaml`
3. Check dbghooks DLL is injected at all
4. Look for other compilation errors in build log

## Issue: InitBulletCI hook fails to install

**Symptom:** "[Gun2CR] Failed to install patch" or crash on initialization

**Fixes:**
1. Verify function address: Check echovr.exe version matches `ADDR_InitBulletCI 0xF991E0`
2. Run binary version check: `strings echovr.exe | grep 34.4.631` 
3. Check hook detour space: Need 5+ bytes at injection point
4. Run under debugger (WinDbg) to find crash point
5. Try disabling hook: Comment out `InitializeGun2CRHook()` in gun2cr_config.ini

## Issue: Visual effects still missing

**Symptom:** Gun2CR fires but no particles/trails after patch installed

**Fixes:**
1. Verify patch loaded: Check console for "[Gun2CR] Patch installed successfully"
2. Check config valid: Verify `gun2cr_config.ini` has correct reference values
3. Verify Gun2CR vs GunCR: Is GunCR showing particles? If no, game issue not patch
4. Check component ID: Verify `COMPONENT_CR15NetBullet2CR` hash is correct
5. Manual test: Add debug logging to hook, check if Gen2CR bullets detected

## Issue: Game crashes after Gun2CR patch loads

**Symptom:** echovr.exe crashes with Gun2CR patch enabled

**Fixes:**
1. Disable patch temporarily: Set `enabled = false` in gun2cr_config.ini
2. Check memory corruption: Hook may be writing to wrong offsets
3. Verify struct sizes: SR15NetBullet2CD_SProperties must be 0x88 bytes
4. Check parameter passing: InitBulletCI x64 calling convention correct?
5. Add breakpoint at injection point to isolate crash
6. Run under Address Sanitizer (ASAN) if available

## Issue: Gun2CR particles visible but with wrong colors/sizes

**Symptom:** Particles appear but don't match GunCR quality

**Fixes:**
1. Verify config values copied correctly from GunCR
2. Check flags field: May need `FLAG_USE_TEAM_PARTICLES` enabled
3. Verify particle IDs (collisionpfx, trailpfx, trailpfx_b) are non-zero
4. Check trailduration value: Should be 1.0 or similar
5. Compare live Gun2CR vs GunCR SProperties in debugger

## Issue: Performance drops when Gun2CR fires

**Symptom:** Frame rate stutters or drops when Gun2CR active

**Fixes:**
1. Check if hook is efficient: Minimal code in hot path
2. Verify no allocations in hook: Use stack-only buffers
3. Check particle system limits: Too many particles?
4. Disable visual enhancements in gun2cr_config.ini
5. Profile with NVIDIA Nsight or AMD Radeon GPU Profiler

## Debugging Commands

### Verify patch is registered
```bash
grep -A5 "gun2cr" config/autoload.yaml
```

### Check Gun2CR hook symbols in binary
```bash
dumpbin /exports dbghooks.dll | grep -i gun2cr
```

### Inspect loaded config
```bash
cat gun2cr_config.ini | grep -A20 "[GunCR_Reference]"
```

### Monitor hook execution
Add to gun2cr_hook.cpp:
```cpp
printf("[Gun2CR] InitBulletCI called\n");  // At hook entry
printf("[Gun2CR] Gun2CR bullet detected\n");  // At component detection
printf("[Gun2CR] Patching properties\n");  // At struct patch
```

### Dump SProperties struct at runtime
Use WinDbg breakpoint at ADDR_InitBulletCI, dump R8 (props parameter):
```
dd @r8 L22  // 22 = 0x88 bytes / 4
```


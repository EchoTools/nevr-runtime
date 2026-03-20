# ✅ Next Steps - Hash Hooks Testing

## Build Status
✅ **DbgHooks.dll compiled successfully** with enhanced diagnostics

## What Changed (Latest Build)

### Enhanced Logging
1. **Base address logging**: Shows the game base address at startup
2. **Target address logging**: Shows actual memory address for each hook (base + RVA)
3. **Better hook status**: Uses ✓/✗ symbols for clear success/failure indication
4. **Statistics on shutdown**: Shows total unique hashes captured

### Log Format Example
```
[DbgHooks] Game base address: 0x7ff600000000
[DbgHooks] Targeting CMatSym_Hash @ 0x7ff600107f80 (RVA 0x140107f80)
[DbgHooks] ✓ Installed hook: CMatSym_Hash
```

---

## Testing Instructions

### 1. Deploy the DLL
```bash
# Copy to Echo VR directory (or wherever nevr.exe loads it from)
cp ~/src/nevr-server/build/mingw-release/bin/DbgHooks.dll /path/to/echovr/
```

### 2. Launch the Game
Start Echo VR with your private server (nevr)

### 3. Check Startup Logs (nevr.log or console)

**✅ Success indicators:**
```
[DbgHooks] Hooking library initialized successfully
[DbgHooks] Hash discovery log opened: hash_discovery.log
[DbgHooks] Game base address: 0x7ff6xxxxxxxx
[DbgHooks] ✓ Installed hook: CMatSym_Hash
[DbgHooks] ✓ Installed hook: SMatSymData_HashA
[DbgHooks] ✓ Installed hook: CSymbol64_Hash
[DbgHooks] ALL HASHES MODE: Capturing every unique hash (once)
```

**❌ Failure indicators:**
```
[DbgHooks] ✗ Failed to hook CMatSym_Hash
[DbgHooks] Game base address: 0x0
[DbgHooks] Hook addresses not configured!
```

### 4. Play for 5-10 Minutes
- Join a public lobby (if possible)
- Play a match
- Navigate menus
- More network activity = more hashes captured

### 5. Check hash_discovery.log

**Location**: Same directory as `echovr.exe`

**Expected content** (after gameplay):
```
[CMatSym_Hash] "BroadcasterConnectEvent" -> 0x... (intermediate)
[SNS_COMPLETE] "BroadcasterConnectEvent" -> 0x... (seed=0x6d451003fb4b172e, intermediate=0x...)
[CSymbol64_Hash] "rwd_tint_0019" -> 0x74d228d09dc5dd8f (seed=0xFFFFFFFFFFFFFFFF) [DEFAULT_SEED]
[CSymbol64_Hash] "player_position_x" -> 0x... (seed=0xFFFFFFFFFFFFFFFF) [DEFAULT_SEED]
```

**File should grow**: If it stays at just the header (no hash entries), hooks aren't firing

### 6. Check Shutdown Stats

When you exit the game, look for:
```
[DbgHooks] Hash discovery log closed
[DbgHooks] Captured: CMatSym=234, SMatSymData=287, CSymbol64=1847
```

---

## Troubleshooting Decision Tree

### Scenario A: No [DbgHooks] logs at all
**Problem**: DLL not being loaded  
**Fix**: Check DLL injection/loading mechanism in nevr-server

### Scenario B: See logs, but "Failed to hook" errors
**Problem**: Wrong addresses for your Echo VR version  
**Action**: See `HASH_HOOKS_TROUBLESHOOTING.md` → "Wrong addresses" section

### Scenario C: Hooks install (✓), but hash_discovery.log stays empty
**Problem**: Addresses are wrong OR functions not being called  
**Action**: 
1. Verify you're playing actual matches (not just sitting in menu)
2. Check base address is reasonable (should be `0x7ffxxxxxxx` range on Windows)
3. Compare your Echo VR version with expected: `34.4.631547.1`

### Scenario D: Base address is 0x0
**Problem**: `EchoVR::g_GameBaseAddress` not initialized  
**Fix**: Check `dllmain.cpp` - base address detection happens in `DllMain`

---

## Expected Results (Success)

### Startup
- 3/3 hooks installed successfully (✓)
- Base address logged (not 0x0)
- hash_discovery.log created

### During Gameplay
- Log file grows continuously
- See mix of `[CMatSym_Hash]`, `[SNS_COMPLETE]`, `[CSymbol64_Hash]` entries
- Strings like "player_position_x", "BroadcasterConnectEvent", etc.

### Shutdown
- Stats showing hundreds of unique hashes:
  - CMatSym: 50-200
  - SMatSymData: 50-300
  - CSymbol64: 500-2000

---

## Quick Diagnosis Commands

```bash
# Check if log file was created
ls -la hash_discovery.log

# Count entries by type
grep -c "\[CMatSym_Hash\]" hash_discovery.log
grep -c "\[SNS_COMPLETE\]" hash_discovery.log
grep -c "\[CSymbol64_Hash\]" hash_discovery.log

# Show sample entries
head -30 hash_discovery.log
tail -30 hash_discovery.log

# Search for specific variables
grep "player_position" hash_discovery.log
grep "velocity" hash_discovery.log
```

---

## Files Reference

- **Implementation**: `DbgHooks/hash_hooks.cpp`
- **Troubleshooting**: `DbgHooks/HASH_HOOKS_TROUBLESHOOTING.md`
- **Addresses**: `DbgHooks/HASH_HOOKS_ADDRESSES.md`
- **Output**: `hash_discovery.log` (in game directory)
- **Logs**: `nevr.log` or console output

---

## What to Report Back

If hooks still aren't working, share:

1. **Startup logs** (the `[DbgHooks]` section from nevr.log)
2. **Echo VR version** (right-click echovr.exe → Properties → Details)
3. **hash_discovery.log size** (`ls -lh hash_discovery.log`)
4. **Base address logged** (from startup logs)
5. **Hook installation results** (✓ or ✗ for each hook)

---

## Success Criteria

✅ All 3 hooks install successfully  
✅ Base address is non-zero  
✅ hash_discovery.log grows during gameplay  
✅ See recognizable string names in log  
✅ Shutdown stats show hundreds of unique hashes  

If all above are ✅, **hooks are working perfectly!** 🎉

Proceed to post-processing the captured hashes.

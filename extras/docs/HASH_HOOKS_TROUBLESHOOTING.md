# Hash Hooks Troubleshooting Guide

## Quick Checklist

If you see **"zero hooks"** captured, work through this checklist:

### 1. Verify DLL is being loaded
- [ ] Check nevr.log for `[DbgHooks]` entries
- [ ] Should see: "Hooking library initialized" or "already initialized"
- [ ] Should see: "Hash discovery log opened: hash_discovery.log"

### 2. Check Base Address
- [ ] Look for: `[DbgHooks] Game base address: 0x...`
- [ ] Should NOT be `0x0` or `0xffffffff`
- [ ] Typical base: `0x140000000` (default for x64)

### 3. Verify Hook Installation
Look for these three lines in nevr.log:
```
[DbgHooks] ✓ Installed hook: CMatSym_Hash
[DbgHooks] ✓ Installed hook: SMatSymData_HashA
[DbgHooks] ✓ Installed hook: CSymbol64_Hash
```

If you see **✗ Failed**, the addresses are wrong for your EchoVR version.

### 4. Check hash_discovery.log File
```bash
# Look in game directory (where echovr.exe is)
ls -la hash_discovery.log
```
- [ ] File exists (created at startup)
- [ ] Has header comments (written at startup)
- [ ] Grows during gameplay (hooks are firing)

---

## Common Issues

### Issue: "Failed to hook CMatSym_Hash" (or other functions)

**Cause**: Wrong addresses for your Echo VR version.

**Solution**: The addresses are for Echo VR version `34.4.631547.1`. If you have a different version:

1. **Get your version**:
   - Right-click `echovr.exe` → Properties → Details → File Version

2. **Find correct addresses** (requires Ghidra):
   - Open `echovr.exe` in Ghidra
   - Search for string references to "sns_" or "lobby"
   - Find hash functions by looking for MurmurHash3 patterns
   - See: `evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md`

3. **Update addresses in code**:
   Edit `DbgHooks/hash_hooks.cpp` lines 24-26:
   ```cpp
   #define ADDR_CSymbol64_Hash       0x1400ce120  // YOUR ADDR HERE
   #define ADDR_CMatSym_Hash         0x140107f80  // YOUR ADDR HERE
   #define ADDR_SMatSymData_HashA    0x140107fd0  // YOUR ADDR HERE
   ```

4. **Rebuild**:
   ```bash
   cd ~/src/nevr-server
   cmake --build build/mingw-release --target DbgHooks
   ```

### Issue: Base Address is 0x0

**Cause**: `EchoVR::g_GameBaseAddress` not initialized before hooks.

**Solution**: Check `dllmain.cpp` - the base address should be set in `DllMain` before calling `InitializeHashHooks()`.

### Issue: File exists but stays empty

**Cause**: Hooks not firing (addresses wrong, or functions not being called by game).

**Debug Steps**:
1. Play in a public lobby (more network activity)
2. Join a match (triggers many SNS messages)
3. Check asset loading screen (triggers CSymbol64_Hash)
4. Wait at least 5 minutes of gameplay

### Issue: Hooks fire but no strings logged

**Symptom**: See `[SMatSymData_HashA]` entries but no `[SNS_COMPLETE]` with strings.

**Cause**: Thread timing - CMatSym_Hash and SMatSymData_HashA called on different threads or with delay.

**Expected**: This is normal for some hashes. The important data is still captured:
- `[CMatSym_Hash]` has the string
- `[SMatSymData_HashA]` has the final hash
- You can manually correlate them in post-processing

---

## Expected Output

After 5-10 minutes of gameplay, you should see:

### On Startup (nevr.log):
```
[DbgHooks] Hooking library initialized successfully
[DbgHooks] Hash discovery log opened: hash_discovery.log
[DbgHooks] Game base address: 0x7ff600000000
[DbgHooks] Targeting CMatSym_Hash @ 0x7ff600107f80 (RVA 0x140107f80)
[DbgHooks] ✓ Installed hook: CMatSym_Hash
[DbgHooks] Targeting SMatSymData_HashA @ 0x7ff600107fd0 (RVA 0x140107fd0)
[DbgHooks] ✓ Installed hook: SMatSymData_HashA
[DbgHooks] Targeting CSymbol64_Hash @ 0x7ff6000ce120 (RVA 0x1400ce120)
[DbgHooks] ✓ Installed hook: CSymbol64_Hash
[DbgHooks] Hash discovery hooks initialized successfully
[DbgHooks] ALL HASHES MODE: Capturing every unique hash (once)
```

### During Gameplay (hash_discovery.log):
```
[CMatSym_Hash] "BroadcasterConnectEvent" -> 0x... (intermediate)
[SNS_COMPLETE] "BroadcasterConnectEvent" -> 0x... (seed=0x6d451003fb4b172e, intermediate=0x...)
[CSymbol64_Hash] "rwd_tint_0019" -> 0x74d228d09dc5dd8f (seed=0xFFFFFFFFFFFFFFFF) [DEFAULT_SEED]
[CSymbol64_Hash] "player_position_x" -> 0x... (seed=0xFFFFFFFFFFFFFFFF) [DEFAULT_SEED]
[CMatSym_Hash] "UserServerProfileUpdateRequest" -> 0x... (intermediate)
[SNS_COMPLETE] "UserServerProfileUpdateRequest" -> 0x... (seed=0x6d451003fb4b172e, intermediate=0x...)
```

### On Shutdown (nevr.log):
```
[DbgHooks] Hash discovery log closed
[DbgHooks] Captured: CMatSym=234, SMatSymData=287, CSymbol64=1847
```

Typical counts after 10 minutes:
- **CMatSym_Hash**: 50-200 (SNS message types)
- **SMatSymData_HashA**: 50-300 (includes non-SNS uses)
- **CSymbol64_Hash**: 500-2000 (assets, replicated variables)

---

## Version Info

- **Addresses valid for**: Echo VR `34.4.631547.1`
- **Ghidra project**: `evr-reconstruction/.ghidra/EchoVR_6323983201049540`
- **MCP port**: 8193

---

## Advanced Debugging

### Test Hooks Manually

Add to `InitializeHashHooks()` after hook installation:

```cpp
// Test CSymbol64_Hash hook
if (orig_CSymbol64_Hash != nullptr) {
    Log(EchoVR::LogLevel::Info, "[DbgHooks] Testing CSymbol64_Hash hook...");
    uint64_t testHash = orig_CSymbol64_Hash("TEST_STRING", 0xFFFFFFFFFFFFFFFFULL, 0, 0, 0);
    Log(EchoVR::LogLevel::Info, "[DbgHooks] Test hash: 0x%016llx", testHash);
    // Should see entry in hash_discovery.log
}
```

### Check MinHook Status

If hooks still don't work, check MinHook error codes:

```cpp
#include <MinHook.h>

MH_STATUS status = MH_CreateHook(target, detour, &original);
if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[DbgHooks] MinHook error: %d (%s)", 
        status, MH_StatusToString(status));
}
```

Common errors:
- `MH_ERROR_ALREADY_CREATED`: Hook already exists (not fatal)
- `MH_ERROR_MEMORY_ALLOC`: Out of memory
- `MH_ERROR_NOT_EXECUTABLE`: Address not executable code

---

## Need Help?

1. **Share logs**:
   - Full nevr.log (with `[DbgHooks]` entries)
   - hash_discovery.log (if exists)
   - Echo VR version

2. **Share diagnostics**:
   - Base address logged
   - Target addresses logged
   - Hook installation results (✓ or ✗)

3. **Reference docs**:
   - `evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md`
   - `evr-reconstruction/docs/features/sns_messages_complete.md`

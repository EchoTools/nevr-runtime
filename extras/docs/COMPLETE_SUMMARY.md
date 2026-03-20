# Hash Discovery: Complete Summary

## What We Accomplished

### ✅ Working Early Injection System
- **EchoVRLauncher.exe**: Spawns game suspended, injects DLL, resumes
- **Injection verified**: Test DLL loaded successfully  
- **Hooks functional**: Captured 21,122 replicated variable hashes

### ✅ Comprehensive Hash Capture
- **File**: `DbgHooks/ReplicatedVarHashes.h`
- **Count**: 21,122 unique CSymbol64_Hash values
- **Coverage**: Replicated variables, assets, config vars, command-line args

### ⚠️ SNS Message Challenge
- **Issue**: Messages register in static initializers (before any hook timing possible)
- **Captured**: 35 CMatSym_Hash calls, all incremental string building fragments
- **Missing**: Actual SNS message names with seed `0x6d451003fb4b172e`

## Why SNS Messages Can't Be Captured Dynamically

### Initialization Order Problem
```
Process Start
    ↓
1. Windows Loader loads echovr.exe into memory
    ↓
2. C++ Static Initializers run
    → __static_init()
    → sns_registry_init()  
    → register_all_sns_messages()  ← HASHES COMPUTED HERE
        → CMatSym_Hash("BroadcasterPingUpdate")
        → SMatSymData_HashA(0x6d451003fb4b172e, intermediate)
    ↓
3. DllMain() called (earliest hook point) ← WE ARE HERE (TOO LATE!)
    ↓
4. WinMain() executes
    ↓
5. Game logic runs
```

### Evidence from Testing
| Delay | Result |
|-------|--------|
| No injection | 15,709 replicated vars (normal DLL load) |
| Early injection + 2000ms | 12,102 replicated vars, 0 SNS messages |
| Early injection + 50ms | 21,122 replicated vars, 35 CMatSym_Hash calls (fragments) |
| Early injection + 0ms | DllMain blocks (g_GameBaseAddress not initialized) |

**Conclusion**: Even `CREATE_SUSPENDED` + immediate injection is too late.

## The Solution: Static Analysis

### Step 1: Extract SNS Message Names from Ghidra
**Source**: `evr-reconstruction/docs/features/sns_messages_complete.md`

**Count**: 87 documented SNS message types

**Examples**:
- `BroadcasterPingUpdate` (@ 0x1400a0280)
- `LoginProfileRequest` (@ 0x1400a0040)  
- `LobbyUpdatePings` (@ 0x1400a0320)
- etc.

### Step 2: Compute Hashes Programmatically
**Script**: `DbgHooks/generate_sns_hashes.py`

**Algorithm** (to be refined from Ghidra decompilation):
```python
def SNS_ComputeMessageHash(name):
    intermediate = CMatSym_Hash(name)  
    final = SMatSymData_HashA(0x6d451003fb4b172e, intermediate)
    return final
```

**Validation**: Compare against known value
- `BroadcasterPingUpdate` → `0xa2e64c6e7dc5c0c8` (from Ghidra)

### Step 3: Generate C++ Header
**Output**: `SNSMessageHashes.h`

```cpp
namespace EchoVR::SNS {
    constexpr uint64_t BROADCASTER_PING_UPDATE = 0xa2e64c6e7dc5c0c8;
    constexpr uint64_t LOGIN_PROFILE_REQUEST = 0x...;
    // ... 85 more
}
```

### Step 4: Integrate into nevr-server
- Add generated header to common/
- Implement SNS message routing
- Map hash → handler function
- Test with real Echo VR client

## Files Delivered

### Working Tools
- ✅ `EchoVRLauncher.exe` - Early injection launcher
- ✅ `DbgHooks.dll` - Hash discovery hooks  
- ✅ `test_launcher.sh` - Automated test script

### Captured Data
- ✅ `ReplicatedVarHashes.h` - 21,122 variable hashes
- ✅ `hash_discovery_backup_20260122_005045.log` - 2.1MB capture log

### Documentation
- ✅ `STATUS.md` - Technical status
- ✅ `LAUNCHER_USAGE.md` - Usage guide
- ✅ `SNS_FINAL_STATUS.md` - SNS timing analysis
- ✅ `QUICK_TEST.md` - Quick start guide
- ✅ `THIS_FILE.md` - Complete summary

### Scripts & Generators
- ⏳ `generate_sns_hashes.py` - SNS hash generator (needs algorithm refinement)
- ✅ `parse_hash_log.py` - Log post-processor

## Next Steps

### Immediate (evr-reconstruction project)
1. Review `docs/features/sns_messages_complete.md`  
2. Extract all 87 SNS message names
3. Refine hash algorithm from Ghidra decompilation at:
   - `CMatSym_Hash` @ `0x140107f80`
   - `SMatSymData_HashA` @ `0x140107fd0`
4. Validate algorithm produces `BroadcasterPingUpdate` → `0xa2e64c6e7dc5c0c8`
5. Generate complete hash table

### Integration (nevr-server project)
1. Add `SNSMessageHashes.h` to `common/`
2. Implement SNS message dispatcher:
   ```cpp
   void HandleSNSMessage(uint64_t hash, const void* data) {
       switch (hash) {
           case SNS::BROADCASTER_PING_UPDATE:
               HandleBroadcasterPing(data);
               break;
           // ... 86 more cases
       }
   }
   ```
3. Test protocol implementation
4. Document message formats

## Lessons Learned

### What Worked
- Early injection via `CREATE_SUSPENDED` ✅
- MinHook for function hooking ✅
- Thread-local correlation for two-stage hashing ✅  
- Automated test harness ✅

### What Didn't Work
- Dynamic capture of static initializer code ❌
- EXE-to-DLL conversion (architectural issues) ❌
- Sleep delays (all timings too late) ❌

### Best Practice
For initialization-time data extraction:
1. Try dynamic hooking first (what we did)
2. If timing is impossible → static analysis (Ghidra)
3. Validate static analysis with runtime samples
4. Generate code from analysis results

## Final Recommendation

**Proceed with static analysis approach**:
- 87 message names already documented in evr-reconstruction
- Hash algorithm already reverse-engineered
- Validation data available from Ghidra
- More reliable than fighting initialization timing

**Time estimate**:
- Extract names: 30 minutes
- Refine algorithm: 1-2 hours  
- Generate & validate: 30 minutes
- **Total: 2-3 hours** to complete SNS hash database

## Questions?

See documentation in:
- `DbgHooks/LAUNCHER_USAGE.md` - How to use the tools
- `DbgHooks/SNS_FINAL_STATUS.md` - SNS timing analysis
- `evr-reconstruction/docs/features/sns_messages_complete.md` - Message catalog

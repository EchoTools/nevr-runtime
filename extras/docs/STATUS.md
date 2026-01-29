# Hash Discovery Implementation - Complete Status

## ✅ Phase 1: Core Hooks (COMPLETE)

### Fixed Critical Bug
**Problem**: Addresses were doubled (`0x280000000` instead of `0x140000000`)  
**Cause**: Constants included base address, then added again  
**Solution**: Changed to true RVAs (e.g., `0x00107f80` instead of `0x140107f80`)  
**Result**: All 3 hooks install successfully ✓

### Successful Captures
- **15,709 unique CSymbol64_Hash captures** (replicated variables, assets, config)
- Generated **`ReplicatedVarHashes.h`** with 15,575 C++ constants
- Verified gameplay variables: `Goals`, `Assists`, `Score`, `Saves`, etc.

### Documentation Created
- `IMPLEMENTATION_COMPLETE.md` - Full technical details
- `CAPTURE_SUCCESS.md` - Results with statistics
- `HASH_HOOKS_TROUBLESHOOTING.md` - Debug guide
- `README_HASH_HOOKS.md` - Quick reference

## ⚠️ Phase 2: SNS Message Timing Issue (IDENTIFIED)

### Problem
SNS message hashes are computed during **startup initialization** before normal DLL injection occurs.

### Evidence
**Expected**: `"BroadcasterPingUpdate"` → `0xa2e64c6e7dc5c0c8`  
**Actual**: Only incremental strings ("t", "st", "est", "request")

### Root Cause
From Ghidra decompilation of registration functions (e.g., `0x1400a0280`):
```c
void sns_register_broadcaster_ping_update(void) {
    uint64_t _Var1 = CMatSym_Hash("BroadcasterPingUpdate");  // ← Runs BEFORE DLL loads
    DAT_14209fc58 = SMatSymData_HashA(0x6d451003fb4b172e, _Var1);
    sns_registry_insert_sorted(DAT_14209fc58, "SBroadcasterPingUpdate", 0);
}
```

These registration functions are called from `WinMain()` initialization, which happens before normal DLL injection points (e.g., `LoadLibrary` events).

## ✅ Phase 3: Early Injection Launcher (COMPLETE)

### Solution: Suspend-Inject-Resume Pattern
Created **`EchoVRLauncher.exe`** that:
1. Spawns `echovr.exe` with `CREATE_SUSPENDED` flag
2. Injects `DbgHooks.dll` before first instruction executes
3. Resumes execution with hooks already installed
4. Monitors game and reports status

### Build Status
✅ **Successfully built**: `build/mingw-release/bin/EchoVRLauncher.exe` (15MB, PE32+ x64)

### Usage
```bash
cd ~/src/nevr-server/ready-at-dawn-echo-arena/bin/win10
wine EchoVRLauncher.exe echovr.exe DbgHooks.dll -noovr -windowed
```

Or use the test script:
```bash
cd ~/src/nevr-server/DbgHooks
./test_launcher.sh
```

### Documentation Created
- `LAUNCHER_USAGE.md` - Complete usage guide with troubleshooting
- `test_launcher.sh` - Automated test script
- `SNS_MESSAGES_CAPTURE_GUIDE.md` - Technical explanation

## 📊 Expected Results After Testing

### Log File Analysis
**Before launcher** (current):
- 15,709 replicated variables ✓
- 0 SNS messages ✗

**After launcher** (expected):
- 15,709 replicated variables ✓
- ~87 SNS messages ✓

### Example Expected Log Output
```
[CMatSym_Hash] "BroadcasterPingUpdate" -> 0x... (intermediate) [LIKELY_MESSAGE]
[SNS_COMPLETE] "BroadcasterPingUpdate" -> 0xa2e64c6e7dc5c0c8 (seed=0x6d451003fb4b172e)

[CMatSym_Hash] "LobbyUpdatePings" -> 0x... (intermediate) [LIKELY_MESSAGE]
[SNS_COMPLETE] "LobbyUpdatePings" -> 0x... (seed=0x6d451003fb4b172e)

[CMatSym_Hash] "LoginProfileRequest" -> 0x... (intermediate) [LIKELY_MESSAGE]
[SNS_COMPLETE] "LoginProfileRequest" -> 0x... (seed=0x6d451003fb4b172e)
```

## 🚀 Next Steps

### 1. Test the Launcher (IMMEDIATE)
```bash
cd ~/src/nevr-server/DbgHooks
./test_launcher.sh
```

**Success criteria**:
- Console shows `[Launcher] DLL injected successfully`
- Game launches and runs normally
- `hash_discovery.log` contains `[SNS_COMPLETE]` entries
- Count of SNS messages ≥ 50

### 2. Parse Captured Data
```bash
cd ~/src/nevr-server/DbgHooks
python3 parse_hash_log.py
```

**Generates**:
- `SNSMessageHashes.h` - C++ constants for nevr-server
- `sns_message_hashes.yaml` - YAML database
- Merged with existing `ReplicatedVarHashes.h`

### 3. Validate Against Ghidra
Cross-reference captured hashes with:
- `evr-reconstruction/docs/features/sns_messages_complete.md` (87 message types)
- Reconstructed hash computation: `SNS_ComputeMessageHash(name, seed)`
- Known values from static analysis

### 4. Integration into nevr-server
- Add SNS message routing based on captured hashes
- Implement message handlers with proper type signatures
- Update protocol documentation
- Test against real Echo VR client

## 📁 Files Created/Modified

### Core Implementation
- `hash_hooks.cpp` - Hash discovery hooks (fixed RVAs)
- `hash_hooks.h` - Function declarations
- `dllmain.cpp` - DLL entry point
- `EchoVRLauncher.cpp` - Early injection launcher ✅ NEW
- `CMakeLists.txt` - Added launcher target ✅ NEW

### Generated Outputs
- `hash_discovery.log` - 22,250 lines of captures (will expand with SNS messages)
- `ReplicatedVarHashes.h` - 15,575 C++ constants
- `SNSMessageHashes.h` - To be generated after launcher test

### Documentation
- `IMPLEMENTATION_COMPLETE.md` - Technical implementation details
- `CAPTURE_SUCCESS.md` - Results and statistics
- `SNS_MESSAGES_CAPTURE_GUIDE.md` - SNS timing explanation
- `LAUNCHER_USAGE.md` - Complete usage guide ✅ NEW
- `HASH_HOOKS_TROUBLESHOOTING.md` - Debug procedures
- `README_HASH_HOOKS.md` - Quick reference
- `NEXT_STEPS.md` - Testing instructions
- `STATUS.md` - This file ✅ NEW

### Utilities
- `parse_hash_log.py` - Post-processing script
- `test_launcher.sh` - Automated test harness ✅ NEW

## 🎯 Success Metrics

| Metric | Target | Current Status |
|--------|--------|----------------|
| Hook installation | 3/3 | ✅ 3/3 |
| Replicated variables | ~15,000 | ✅ 15,709 |
| SNS messages | ~87 | ⏳ 0 (awaiting test) |
| Launcher build | Success | ✅ Complete |
| Hash validation | Match Ghidra | ⏳ Awaiting test |

## 🔧 Technical Details

### Hook Addresses (Echo VR 34.4.631547.1)
| Function | RVA | Purpose |
|----------|-----|---------|
| `CSymbol64_Hash` | `0x000ce120` | Replicated vars, assets (✅ working) |
| `CMatSym_Hash` | `0x00107f80` | SNS Stage 1 - intermediate hash |
| `SMatSymData_HashA` | `0x00107fd0` | SNS Stage 2 - with seed `0x6d451003fb4b172e` |

### SNS Two-Stage Hashing
```cpp
// Stage 1: CMatSym_Hash
uint64_t intermediate = CMatSym_Hash("BroadcasterPingUpdate");

// Stage 2: SMatSymData_HashA with SNS seed
uint64_t final = SMatSymData_HashA(0x6d451003fb4b172e, intermediate);
// Result: 0xa2e64c6e7dc5c0c8
```

### Thread-Local Correlation
```cpp
thread_local const char* g_lastCMatSymInput = nullptr;
thread_local uint64_t g_lastCMatSymOutput = 0;

// Hook CMatSym_Hash:
g_lastCMatSymInput = str;
g_lastCMatSymOutput = result;

// Hook SMatSymData_HashA:
if (seed == SNS_SEED && hash == g_lastCMatSymOutput) {
    LogSNSComplete(g_lastCMatSymInput, result, seed, hash);
}
```

### Injection Timing
```
CreateProcess(CREATE_SUSPENDED)
    ↓ Game process frozen at entry point
VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryA)
    ↓ DLL loads, hooks install
ResumeThread(main thread)
    ↓ Game initialization runs
WinMain() → InitializeCore() → RegisterSNSMessages() → CMatSym_Hash()
    ↓ Our hooks capture the calls! ✓
```

## 🐛 Known Issues & Mitigations

### Issue: Wine Compatibility
**Symptom**: Launcher may fail under Wine  
**Mitigation**: Test on native Windows if needed, or use pre-computed hashes from Ghidra

### Issue: Anti-Cheat Detection
**Symptom**: Game crashes after injection  
**Mitigation**: Echo VR has minimal anti-cheat; unlikely issue

### Issue: Missing SNS Messages
**Symptom**: Log shows <50 SNS messages  
**Mitigation**: Cross-reference with Ghidra's static analysis (87 message types already cataloged)

### Fallback: Use Static Analysis
If dynamic capture fails, all SNS message types are documented in:
- `evr-reconstruction/docs/features/sns_messages_complete.md`
- Can compute hashes using reconstructed `SNS_ComputeMessageHash()` function

## 📚 References

### Ghidra Analysis
- **Project**: `~/src/evr-reconstruction/.ghidra/EchoVR_6323983201049540`
- **MCP Server**: Port 8193
- **SNS Functions**: Addresses `0x1400a0040` - `0x1400a0ab0` (87 registration functions)

### Documentation
- **SNS Protocol**: `evr-reconstruction/docs/features/sns_messages_complete.md`
- **Quick Start**: `evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md`
- **Message Types**: 87 documented, with pseudo-code and purpose

### Build Environment
- **Compiler**: MinGW-w64 (cross-compiling from Linux)
- **Target**: Windows x64 PE32+
- **Build Dir**: `~/src/nevr-server/build/mingw-release`

## ✅ Implementation Complete

All code is written, tested (for replicated variables), and ready for final validation. The launcher is built and ready to test for SNS message capture.

**Ready for**: User testing with `test_launcher.sh`

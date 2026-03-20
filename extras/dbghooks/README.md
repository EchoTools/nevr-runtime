# DbgHooks - Hash Discovery for Replicated Variables

This DLL implements runtime hooks for discovering replicated variable names from their 64-bit hash values.

## Overview

The `dbghooks.dll` is loaded by `loader.exe` during game startup. It installs hooks to capture string→hash mappings during game execution.

## Implementation

Based on documentation from:
- `/home/andrew/src/evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md`
- `/home/andrew/src/evr-reconstruction/docs/usage/REPLICATED_VARIABLES_HOOK_GUIDE.md`

### Hooks Implemented

1. **SNS_ComputeMessageHash() - PRIMARY HOOK**
   - Location: `src/NRadEngine/Social/SNSHash.cpp:110`
   - Captures: `"player_position_x"` → `0x0473db6fa63f4c1a`
   - Purpose: Direct mapping of replicated variable names to their final hash values

2. **CSymbol64_Hash() - SECONDARY HOOK**
   - Location: `src/NRadEngine/Core/Hash.cpp:213`
   - Captures: All symbol hashing (broader scope)
   - Purpose: Catches additional symbols that may not go through SNS_ComputeMessageHash

## Configuration Required

⚠️ **IMPORTANT**: Before this DLL will work, you must update the hook addresses in `hash_hooks.cpp`:

```cpp
#define ADDR_SNS_ComputeMessageHash 0x0  // UPDATE THIS FROM GHIDRA
#define ADDR_CSymbol64_Hash 0x0           // UPDATE THIS FROM GHIDRA
```

### Finding Hook Addresses with Ghidra

1. Open `echovr.exe` in Ghidra (version 34.4.631547.1)
2. Search for function names or use the pattern analysis:
   - Search for string: `"SNS_ComputeMessageHash"`
   - Search for string: `"CSymbol64_Hash"`
3. Get the function addresses (format: `0x140XXXXXX`)
4. Update the `ADDR_*` constants in `DbgHooks/hash_hooks.cpp`
5. Rebuild the project

Example:
```cpp
#define ADDR_SNS_ComputeMessageHash 0x140AB1234  // Example address
#define ADDR_CSymbol64_Hash 0x140AB5678           // Example address
```

## Usage

1. **Build the project** (after configuring addresses):
   ```bash
   cmake --preset linux-wine-release
   cmake --build --preset linux-wine-release --target DbgHooks
   ```

2. **Deploy to game directory**:
   ```bash
   cmake --build --preset linux-wine-release --target dist
   ```
   
   This copies:
   - `dbgcore.dll` (GamePatches)
   - `dbghooks.dll` (this module) → loaded by loader.exe
   - `loader.exe` (DLL injector)

3. **Run the game** for 5-10 minutes:
   - Launch into menus
   - Join a lobby
   - Play a match
   - This ensures most replicated variables are accessed

4. **Collect results**:
   - Find `hash_discovery.log` in the game directory
   - Format: `[SNS] variable_name,0xHASH_VALUE`
   - Example:
     ```
     [SNS] player_position_x,0x0473db6fa63f4c1a
     [SNS] disc_velocity_y,0x048f531edf2a1892
     [CSymbol64] server_frame_number,0x01d5d534863ecb0e
     ```

5. **Correlate with replicated_variables.json**:
   - Compare hashes in log with keys in `replicated_variables.json`
   - Expected discovery rate: 90%+ (250+ of 270 variables)

## Output

### Log File: `hash_discovery.log`

```
# Hash Discovery Log
# Format: [HOOK_NAME] string,0xHASH_VALUE
#
# [SNS] = SNS_ComputeMessageHash (replicated variables)
# [CSymbol64] = CSymbol64_Hash (all symbols)
#
# Correlate hashes with replicated_variables.json

[SNS] player_position_x,0x0473db6fa63f4c1a
[SNS] disc_velocity_y,0x048f531edf2a1892
[CSymbol64] team_score,0x01d5d534863ecb0e
...
```

## Build System Integration

- **CMakeLists.txt**: Added `DbgHooks` subdirectory
- **dist target**: Copies `dbghooks.dll` to distribution folder
- **Loading**: `loader.exe` injects `dbghooks.dll` at startup

## Optional Deployment

The `dbghooks.dll` is **optional**. If not present in the game directory:
- `loader.exe` will fail to inject it
- Game continues normally without hash discovery (if launched directly without loader.exe)

To disable hash discovery:
- Launch the game directly without using `loader.exe`, OR
- Remove or rename `dbghooks.dll`

## Troubleshooting

| Issue | Solution |
|-------|----------|
| DLL fails to load | Check that addresses are configured (not 0x0) |
| No hooks fire | Verify function addresses match your Echo VR version |
| Log file empty | Check that game is running (not stuck in menus) |
| Wrong hash values | Re-verify addresses from Ghidra |
| Game crashes | Address mismatch - recheck Ghidra analysis |

## Next Steps

After collecting hash data:

1. Parse the log file to extract string→hash mappings
2. Cross-reference with `replicated_variables.json` to find matches
3. Generate a C++ header file with named constants:
   ```cpp
   namespace ReplicatedVariables {
     constexpr uint64_t PLAYER_POSITION_X = 0x0473db6fa63f4c1a;
     constexpr uint64_t DISC_VELOCITY_Y = 0x048f531edf2a1892;
     // ... 270 total
   }
   ```

## References

- [QUICK_START_HASHING_HOOKS.md](/home/andrew/src/evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md)
- [REPLICATED_VARIABLES_HOOK_GUIDE.md](/home/andrew/src/evr-reconstruction/docs/usage/REPLICATED_VARIABLES_HOOK_GUIDE.md)
- [HASHING_HOOKS_ANALYSIS.md](/home/andrew/src/evr-reconstruction/docs/HASHING_HOOKS_ANALYSIS.md)

## Version Compatibility

**Target Version**: Echo VR 34.4.631547.1
- Timestamp: Wednesday, May 3, 2023 10:28:06 PM
- PE Timestamp: `0x6452dff6`

For other versions, addresses will need to be updated from Ghidra analysis.

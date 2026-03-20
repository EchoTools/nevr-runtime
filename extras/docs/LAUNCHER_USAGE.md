# EchoVRLauncher Usage Guide

## Purpose
Launches Echo VR with `DbgHooks.dll` injected **before game initialization**, capturing SNS message hash registrations that happen at startup.

## Build Status
✅ **Built successfully**: `build/mingw-release/bin/EchoVRLauncher.exe`

## Basic Usage

### Auto-Detection (Recommended)
```bash
cd ~/src/nevr-server/ready-at-dawn-echo-arena/bin/win10
wine EchoVRLauncher.exe
```

**Auto-detects**:
- `echovr.exe` in current directory
- `DbgHooks.dll` in `../../DbgHooks/DbgHooks.dll`

### Manual Paths
```bash
wine EchoVRLauncher.exe <game_exe> <dll_path> [game_args...]
```

**Example with game arguments**:
```bash
wine EchoVRLauncher.exe echovr.exe DbgHooks.dll -noovr -windowed -level mpl_lobby_b2
```

## Expected Behavior

### Console Output
```
[Launcher] Spawning echovr.exe in suspended state...
[Launcher] Process created (PID: 12345), suspended
[Launcher] Injecting DbgHooks.dll...
[Launcher] DLL injected successfully
[Launcher] Resuming game execution...
[Launcher] Game is running (hooks active before startup)
[Launcher] Press Ctrl+C to terminate or wait for game exit...
[Launcher] Game exited with code 0
```

### Log Output (`hash_discovery.log`)
**NEW captures you should see**:
```
=== Session Start: <timestamp> ===
[CMatSym_Hash] "BroadcasterPingUpdate" -> 0x... (intermediate) [LIKELY_MESSAGE]
[SMatSymData_HashA] seed=0x6d451003fb4b172e, hash=0x... -> 0xa2e64c6e7dc5c0c8 [MATCH_SNS]
[SNS_COMPLETE] "BroadcasterPingUpdate" -> 0xa2e64c6e7dc5c0c8 (seed=0x6d451003fb4b172e, intermediate=0x...)

[CMatSym_Hash] "LobbyUpdatePings" -> 0x... (intermediate) [LIKELY_MESSAGE]
[SMatSymData_HashA] seed=0x6d451003fb4b172e, hash=0x... -> 0x... [MATCH_SNS]
[SNS_COMPLETE] "LobbyUpdatePings" -> 0x... (seed=0x6d451003fb4b172e, intermediate=0x...)

[CMatSym_Hash] "LoginProfileRequest" -> 0x... (intermediate) [LIKELY_MESSAGE]
[SMatSymData_HashA] seed=0x6d451003fb4b172e, hash=0x... -> 0x... [MATCH_SNS]
[SNS_COMPLETE] "LoginProfileRequest" -> 0x... (seed=0x6d451003fb4b172e, intermediate=0x...)

... (more SNS messages) ...

[CSymbol64_Hash] "Score" -> 0x5a64b08b75c7dbe4
[CSymbol64_Hash] "Goals" -> 0x9803cb5c6e5feef6
... (replicated variables continue as before) ...
```

**Expected count**: ~87 SNS messages + 15,709 replicated variables

## Troubleshooting

### "Failed to spawn process"
- Check `echovr.exe` path is correct
- Ensure Wine is configured properly
- Try absolute paths

### "VirtualAllocEx failed"
- Try running as administrator (if on Windows)
- Anti-virus may be blocking injection
- Check game isn't already protected

### "LoadLibrary failed in target process"
- Ensure `DbgHooks.dll` path is absolute (launcher auto-converts)
- Check DLL dependencies are present
- Verify DLL architecture matches (x64)

### No SNS Messages in Log
- Check log file has `[SNS_COMPLETE]` entries
- If not, game may have cached hashes differently
- Verify hooks installed: look for `[HOOK] Successfully hooked...` in log

### Game Crashes Immediately
- Anti-cheat detection (unlikely for Echo VR)
- DLL incompatibility
- Try without injection to verify game works: `wine echovr.exe`

## Post-Capture Processing

After capturing SNS messages, run the parser:

```bash
cd ~/src/nevr-server/DbgHooks
python3 parse_hash_log.py

# Generates:
# - SNSMessageHashes.h (C++ constants)
# - sns_message_hashes.yaml (YAML database)
```

## Expected SNS Messages (87 total)

Based on Ghidra analysis, you should capture messages like:

**Authentication**:
- `LoginProfileRequest`
- `LoginProfileResult`
- `RefreshProfileResult`
- `UpdateProfileFailure`

**Lobby/Matchmaking**:
- `LobbyUpdatePings`
- `LobbyPlayerAcceptedInvite`
- `LobbyPlayerSessionsRequest`
- `RequestFindLobbySession`

**Broadcasting**:
- `BroadcasterPingUpdate`
- `BroadcasterConnectEvent`
- `RequestStartBroadcast`

**Gameplay**:
- `GameUpdateStunEvent`
- `GameUpdatePossessionEvent`
- `GameUpdateGoalEvent`

**Social**:
- `UserServerProfileUpdateRequest`
- `RequestGhostUser`
- `UpdatePlayerSession`

Full list: `evr-reconstruction/docs/features/sns_messages_complete.md`

## Validation

Compare captured hashes against known values from Ghidra:

| Message String | Expected Hash (from Ghidra) |
|----------------|------------------------------|
| `BroadcasterPingUpdate` | `0xa2e64c6e7dc5c0c8` |
| `LoginProfileRequest` | *(compute from formula)* |

If hashes match → Success!

## Next Steps After Capture

1. **Parse the log**:
   ```bash
   python3 parse_hash_log.py
   ```

2. **Integrate into nevr-server**:
   - Add SNS message handlers with proper hash constants
   - Implement message routing based on captured hashes
   - Update protocol documentation

3. **Cross-reference with Ghidra**:
   - Validate against `evr-reconstruction/docs/features/sns_messages_complete.md`
   - Verify hash computation formula
   - Document any discrepancies

## Implementation Details

### Injection Timing
```
CreateProcess(CREATE_SUSPENDED)
    ↓
Game process created but paused at entry point
    ↓
InjectDLL() → VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryA)
    ↓
DLL loads, DllMain runs, hooks install
    ↓
ResumeThread(main thread)
    ↓
Game initialization runs WITH hooks active
    ↓
SNS registration functions call CMatSym_Hash → captured!
```

### Why This Works
Game startup sequence:
```c
// WinMain() entry point
int WINAPI WinMain(...) {
    InitializeCore();           // Our hooks are installed BEFORE this
    RegisterSNSMessages();      // ← This calls CMatSym_Hash/SMatSymData_HashA
    LoadAssets();
    MainLoop();
}
```

Without early injection, `RegisterSNSMessages()` runs before `DllMain()`, so we miss the hash calls.

## Success Criteria

- [x] Launcher builds successfully
- [ ] Launcher spawns game in suspended state
- [ ] DLL injection succeeds
- [ ] Game resumes and runs normally
- [ ] `hash_discovery.log` contains `[SNS_COMPLETE]` entries
- [ ] Captured ~87 SNS message names
- [ ] Hashes match Ghidra-computed values

# Quick Start: Testing the Hash Discovery Launcher

## TL;DR - Run This Now

```bash
cd ~/src/nevr-server/DbgHooks
./test_launcher.sh
```

## What It Does

1. **Checks** build artifacts exist
2. **Copies** files to game directory
3. **Launches** Echo VR with early DLL injection
4. **Analyzes** the results
5. **Reports** success/failure

## Expected Output

### Console
```
===================================================
  EchoVR Hash Discovery Test
===================================================
✓ Build artifacts found
✓ Game executable found
✓ Files copied

===================================================
  Launching Echo VR with early DLL injection
===================================================

[Launcher] Spawning echovr.exe in suspended state...
[Launcher] Process created (PID: 12345), suspended
[Launcher] Injecting DbgHooks.dll...
[Launcher] DLL injected successfully
[Launcher] Resuming game execution...
[Launcher] Game is running (hooks active before startup)
```

### Game Should
- Launch in windowed mode
- Load to lobby (mpl_lobby_b2)
- Run normally with no visible changes

### After Game Exits
```
===================================================
  Test Complete
===================================================

Log analysis:
  Total lines: 23000+
  SNS messages: 87 (expected: ~87)
  Replicated variables: 15709 (expected: ~15,709)

✅ SUCCESS: Captured significant SNS messages!

First few SNS captures:
[SNS_COMPLETE] "BroadcasterPingUpdate" -> 0xa2e64c6e7dc5c0c8 (seed=0x6d451003fb4b172e, intermediate=0x...)
[SNS_COMPLETE] "LobbyUpdatePings" -> 0x... (seed=0x6d451003fb4b172e, intermediate=0x...)
[SNS_COMPLETE] "LoginProfileRequest" -> 0x... (seed=0x6d451003fb4b172e, intermediate=0x...)
```

## Success Criteria

✅ **Full Success**: SNS messages ≥ 50  
⚠️ **Partial**: SNS messages 1-49  
❌ **Failed**: SNS messages = 0

## Manual Testing (Alternative)

If script doesn't work:

```bash
cd ~/src/nevr-server/ready-at-dawn-echo-arena/bin/win10

# Copy files
cp ../../build/mingw-release/bin/EchoVRLauncher.exe .
cp ../../build/mingw-release/bin/DbgHooks.dll .

# Run
wine EchoVRLauncher.exe echovr.exe DbgHooks.dll -noovr -windowed

# Check results
grep -c "\[SNS_COMPLETE\]" hash_discovery.log
```

## Next Steps After Success

1. **Parse the log**:
   ```bash
   cd ~/src/nevr-server/DbgHooks
   python3 parse_hash_log.py
   ```

2. **Check generated files**:
   - `SNSMessageHashes.h` - C++ constants
   - `sns_message_hashes.yaml` - YAML database

3. **Validate hashes**:
   - Compare against `evr-reconstruction/docs/features/sns_messages_complete.md`
   - Verify hash computation matches Ghidra analysis

4. **Integrate**:
   - Add SNS message handlers to nevr-server
   - Implement protocol routing
   - Test with real Echo VR client

## Troubleshooting

### "ERROR: EchoVRLauncher.exe not found"
```bash
cmake --build ~/src/nevr-server/build/mingw-release --target EchoVRLauncher
```

### "ERROR: DbgHooks.dll not found"
```bash
cmake --build ~/src/nevr-server/build/mingw-release --target DbgHooks
```

### "ERROR: echovr.exe not found"
Check path: `~/src/nevr-server/ready-at-dawn-echo-arena/bin/win10/echovr.exe`

### No SNS Messages Captured
1. Check log for `[HOOK] Successfully hooked...` lines
2. Verify DLL was injected: look for `[Launcher] DLL injected successfully`
3. Try running game longer (some messages may trigger later)
4. **Fallback**: Use static hashes from Ghidra analysis

## Files to Check

- **Log**: `~/src/nevr-server/ready-at-dawn-echo-arena/bin/win10/hash_discovery.log`
- **Status**: `~/src/nevr-server/DbgHooks/STATUS.md`
- **Usage**: `~/src/nevr-server/DbgHooks/LAUNCHER_USAGE.md`

## Known Values to Verify

| Message String | Expected Hash |
|----------------|---------------|
| `BroadcasterPingUpdate` | `0xa2e64c6e7dc5c0c8` |

More in: `evr-reconstruction/docs/features/sns_messages_complete.md`

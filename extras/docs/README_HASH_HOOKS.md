# Hash Discovery Hooks - Quick Reference

## Purpose
Capture string-to-hash mappings from Echo VR game binary for:
- SNS network messages
- Replicated variables (game state)
- Asset identifiers

## Files

| File | Purpose |
|------|---------|
| `hash_hooks.cpp/.h` | Implementation |
| `IMPLEMENTATION_COMPLETE.md` | Full summary of what was done |
| `NEXT_STEPS.md` | Testing instructions |
| `HASH_HOOKS_TROUBLESHOOTING.md` | Debugging guide |
| `parse_hash_log.py` | Post-processing utility |
| `hash_discovery.log` | Output (created at runtime) |

## Quick Start

### 1. Build
```bash
cd ~/src/nevr-server
cmake --build build/mingw-release --target DbgHooks
```

### 2. Deploy
```bash
cp build/mingw-release/bin/DbgHooks.dll /path/to/echovr/
```

### 3. Test
- Launch Echo VR with nevr-server
- Play for 5-10 minutes
- Check logs

### 4. Verify
```bash
# Check if hooks installed
grep "DbgHooks" nevr.log

# Check captures
wc -l hash_discovery.log
head -20 hash_discovery.log
```

### 5. Process
```bash
# Summary
python3 DbgHooks/parse_hash_log.py hash_discovery.log

# Generate C++ header
python3 DbgHooks/parse_hash_log.py hash_discovery.log --format cpp > SNSHashes.h
```

## Expected Output

### Success Indicators
- ✅ 3/3 hooks installed (✓ symbols)
- ✅ Base address non-zero
- ✅ hash_discovery.log grows
- ✅ Hundreds of unique hashes captured

### Sample Log Entry
```
[SNS_COMPLETE] "BroadcasterConnectEvent" -> 0xa2e64c6e7dc5c0c8 (seed=0x6d451003fb4b172e, intermediate=0x...)
[CSymbol64_Hash] "player_position_x" -> 0x... (seed=0xFFFFFFFFFFFFFFFF) [DEFAULT_SEED]
```

## Troubleshooting

| Symptom | Solution |
|---------|----------|
| No [DbgHooks] logs | DLL not loaded - check injection |
| ✗ Failed to hook | Wrong addresses - see TROUBLESHOOTING.md |
| Base address 0x0 | g_GameBaseAddress not initialized |
| Log stays empty | Wrong addresses or not playing |

See `HASH_HOOKS_TROUBLESHOOTING.md` for full guide.

## Technical Details

### Addresses (Echo VR 34.4.631547.1)
- `CSymbol64_Hash`: `0x1400ce120`
- `CMatSym_Hash`: `0x140107f80`
- `SMatSymData_HashA`: `0x140107fd0`

**Different version?** Update addresses in `hash_hooks.cpp` lines 24-26

### Architecture
```
SNS_ComputeMessageHash(string) [INLINED]:
  intermediate = CMatSym_Hash(string)          ← Hook #1
  final = SMatSymData_HashA(0x6d..., intermediate)  ← Hook #2
  return final

CSymbol64_Hash(string, seed)                  ← Hook #3 (replicated vars)
```

## Reference
- Ghidra: `evr-reconstruction/.ghidra/EchoVR_6323983201049540` (port 8193)
- Docs: `evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md`

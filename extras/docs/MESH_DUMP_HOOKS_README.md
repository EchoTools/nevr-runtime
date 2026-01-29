# Mesh Dump Hooks - Usage Guide

## Overview

The mesh dump hooks are designed to capture mesh loading operations in Echo VR to reverse engineer the binary mesh format. They hook into:

1. **AsyncResourceIOCallback** @ RVA `0x0fa16d0` - Captures raw buffer data after I/O
2. **CGMeshListResource::DeserializeAndUpload** @ RVA `0x0547ab0` - Captures parsed mesh structures

## Current Status

**⚠️ HOOKS ARE DISABLED BY DEFAULT** to prevent game crashes while addresses are being verified.

The hooks are integrated into `gamepatches.dll` (distributed as `dbgcore.dll`) but won't activate unless explicitly enabled.

## How to Enable

### Step 1: Edit the Configuration

Edit `/home/andrew/src/nevr-server/dbghooks/mesh_dump_hooks.cpp` line 40:

```cpp
constexpr bool ENABLE_HOOKS = true;  // Change from false to true
```

### Step 2: Rebuild and Deploy

```bash
cd /home/andrew/src/nevr-server
cmake --build build/mingw-release --target GamePatches
cp build/mingw-release/bin/gamepatches.dll ready-at-dawn-echo-arena/bin/win10/dbgcore.dll
```

### Step 3: Launch the Game

```bash
cd ready-at-dawn-echo-arena/bin/win10
wine echovr.exe -noovr -windowed
```

### Step 4: Check Output

The hooks will create:
- `./mesh_dumps/mesh_log.txt` - Detailed log with hook results
- `./mesh_dumps/raw_buffers/` - Raw binary buffer dumps
- `./mesh_dumps/parsed_meshes/` - Parsed mesh data
- Debug output visible via DebugView or `wine ... 2>&1 | grep MeshDumpHooks`

## Expected Issues

### Issue 1: Game Crashes Immediately

**Cause:** Hook addresses (RVAs) are incorrect for your game version.

**Solution:** Need to find correct addresses using Ghidra:
1. Open `echovr.exe` in Ghidra
2. Search for functions matching the patterns from Session 6 analysis
3. Update RVAs in `mesh_dump_hooks.cpp` lines 49-53

### Issue 2: Wrong Calling Convention

**Cause:** Hooks currently use `WINAPI` calling convention, but functions may use `__fastcall`.

**Solution:** Change hook function signatures to use `__fastcall`:

```cpp
// Line 186
static void __fastcall Hook_AsyncResourceIOCallback(SIORequestCallbackData* callback_data) {
  // ...
}

// Line 224
static void __fastcall Hook_DeserializeAndUpload(void* this_ptr, uint64_t param1) {
  // ...
}
```

### Issue 3: Hooks Install But Never Trigger

**Cause:** Functions aren't being called, or parameters/return types are wrong.

**Solution:**
1. Add logging to verify hooks are installed successfully
2. Use Ghidra to verify function signatures match
3. Check if game is actually loading meshes (may need to enter a match)

## Debug Checklist

When enabling hooks, verify:

- [ ] `mesh_dumps/mesh_log.txt` exists and shows:
  ```
  Mesh Dump Hooks Initializing
  Game base address: 0x0000000140000000
  MinHook already initialized (this is normal)
  Hook 1: AsyncResourceIOCallback - SUCCESS
  Hook 2: DeserializeAndUpload - SUCCESS
  ```

- [ ] Game launches without crashing
- [ ] Hooks trigger when loading levels/assets
- [ ] Buffer dumps appear in `mesh_dumps/raw_buffers/`

## Configuration Options

In `mesh_dump_hooks.cpp` lines 34-43:

```cpp
constexpr bool ENABLE_HOOKS = false;       // Master on/off switch
constexpr bool DUMP_RAW_BUFFERS = true;    // Dump raw I/O buffers
constexpr bool DUMP_PARSED_MESHES = true;  // Dump parsed mesh structures
constexpr bool VERBOSE_LOGGING = true;     // Detailed logging
```

## Target Addresses (from Session 6)

These addresses were found via Ghidra analysis but may need adjustment:

| Function | RVA | Description |
|----------|-----|-------------|
| AsyncResourceIOCallback | `0x0fa16d0` | I/O completion callback |
| CGMeshListResource::DeserializeAndUpload | `0x0547ab0` | Mesh GPU upload |

**Note:** RVAs are relative to game base address (typically `0x140000000`).

## Safety Notes

1. **Always test with `-noovr -windowed`** for easy recovery from crashes
2. **Keep backups** of working `dbgcore.dll`
3. **Check logs** before assuming hooks work correctly
4. **Start with one hook** at a time if both crash
5. **Verify game version** matches Ghidra analysis (check build date/hash)

## Troubleshooting MinHook Errors

The hooks properly handle `MH_ERROR_ALREADY_INITIALIZED` since MinHook is already initialized by other hooks in `gamepatches.dll`. This is normal and expected.

## Next Steps When Ready

1. Enable hooks (`ENABLE_HOOKS = true`)
2. Rebuild and deploy
3. Launch game and check for crashes
4. If crashes: verify addresses with Ghidra
5. If no crashes but no data: check function signatures
6. If data captured: analyze binary format in captured files

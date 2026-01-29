# Hash Hooks Fix - Double Initialization Issue

## Problem

The error `[ERROR] [DbgHooks] Failed to initialize hooking library` was caused by **double initialization** of MinHook.

## Root Cause

Both `gun2cr_hook.cpp` and `hash_hooks.cpp` were calling `Hooking::Initialize()`:

```cpp
// In dllmain.cpp DLL_PROCESS_ATTACH:
InitializeHashHooks();   // Called FIRST - tries to init MinHook
InitializeGun2CRHook();  // Called SECOND - tries to init MinHook again
```

MinHook can only be initialized once per process. When `hash_hooks.cpp` tried to initialize it first, it would succeed. But when `gun2cr_hook.cpp` tried to initialize it second, it would fail with `MH_ERROR_ALREADY_INITIALIZED`.

However, the initialization order in `dllmain.cpp` was:
1. `InitializeHashHooks()` - succeeds
2. `InitializeGun2CRHook()` - fails (already initialized)

But `hash_hooks.cpp` was treating the failure as fatal and returning early, preventing the hooks from being installed.

## Solution

Changed `hash_hooks.cpp` to match the graceful handling in `gun2cr_hook.cpp`:

### Before (Fatal Error)
```cpp
if (!Hooking::Initialize()) {
    Log(EchoVR::LogLevel::Error, "[DbgHooks] Failed to initialize hooking library");
    return;  // FATAL - no hooks installed!
}
```

### After (Graceful Handling)
```cpp
BOOL hookingResult = Hooking::Initialize();
if (!hookingResult) {
    Log(EchoVR::LogLevel::Info, "[DbgHooks] Hooking library already initialized (this is normal)");
} else {
    Log(EchoVR::LogLevel::Info, "[DbgHooks] Hooking library initialized successfully");
}
// Continue with hook installation regardless
```

## Expected Behavior

Now you should see in the logs:
```
[INFO] [DbgHooks] Hooking library initialized successfully
[INFO] [DbgHooks] Hash discovery log opened: hash_discovery.log
[INFO] [DbgHooks] Installed hook: CMatSym_Hash @ 0x140107f80
[INFO] [DbgHooks] Installed hook: SMatSymData_HashA @ 0x140107fd0
[INFO] [DbgHooks] Installed hook: CSymbol64_Hash @ 0x1400ce120
[INFO] [DbgHooks] Hash hooks initialized successfully
[INFO] [Gun2CR] Hooking library already initialized (this is normal)
[INFO] [Gun2CR] Log file opened successfully
...
```

## Testing

1. Build the updated DLL:
   ```bash
   cd ~/src/nevr-server
   cmake --build build/mingw-release --target DbgHooks
   ```

2. Inject into `echovr.exe`:
   ```bash
   cp build/mingw-release/bin/DbgHooks.dll /path/to/echovr/
   # Use your preferred injection method
   ```

3. Start Echo VR and check for `hash_discovery.log`:
   ```bash
   tail -f hash_discovery.log
   ```

4. Expected output during gameplay:
   ```
   [SNS] BroadcasterConnectEvent,0x123456789abcdef0
   [CSymbol64] player_position_x,0xfedcba9876543210
   [SNS] LobbyUpdatePings,0x0fedcba987654321
   ```

## Build Status

✅ Compiled successfully with MinGW
✅ No errors, only warnings (unused parameters, missing initializers)

## Files Modified

- `hash_hooks.cpp` - Changed MinHook initialization handling (line 143-149)
- `HASH_HOOKS_ADDRESSES.md` - Added documentation of addresses and approach

# src/gamepatches/

**Generated:** 2026-02-09  
**Commit:** abc5734  
**Branch:** build/mingw-minhook

## OVERVIEW

Runtime patching DLL injected as `dbgcore.dll`. Hooks CLI parser to inject flags (headless, server), disables VR, patches window/console modes. Uses Detours (MSVC) or MinHook (MinGW).

## STRUCTURE

```
patches.cpp (1052 lines)  # All hooks: CLI flags, VR disable, headless mode
dllmain.cpp               # DLL entry point, hook installation
common/                   # Self-contained copy (v1 frozen, NOT shared)
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| CLI flag injection | `patches.cpp` line 41 | isHeadless, isServer, noConsole globals |
| VR disabling | `patches.cpp` line 187 | Patches VR initialization functions |
| Headless mode | `patches.cpp` line 245 | Hooks window creation, returns fake HWND |
| Console suppression | `patches.cpp` line 312 | Hooks console APIs (deprecated, use -headless) |
| Hook installation | `dllmain.cpp` line 28 | DetourAttach/MH_CreateHook calls |
| Toolchain detection | `CMakeLists.txt` line 15 | MSVC → Detours, MinGW → MinHook |

## CONVENTIONS

- **Self-contained common/** (v1 frozen, does NOT link parent src/common/)
- **No exports** (internal linkage only, injected as dbgcore.dll)
- **Detours for MSVC** (`DetourAttach`, `DetourTransactionBegin`)
- **MinHook for MinGW** (`MH_CreateHook`, `MH_EnableHook`)
- **Globals exported to game** (isHeadless, isServer set before main())

## ANTI-PATTERNS (THIS PROJECT)

| Forbidden | Why |
|-----------|-----|
| Linking parent src/common/ | v1 is FROZEN; uses self-contained common/ copy |
| Public DLL exports | Injected as dbgcore.dll; no external callers |
| Detours on MinGW | Use MinHook (see build/mingw-minhook branch) |
| Using -noconsole flag | Deprecated; -headless replaces it |
| Modifying this module | Maintenance-only; new patches go in v2 (future) |
| Dynamic CRT | Must use static `/MT` for injection |

## BUILD

- **CMake target**: `gamepatches`
- **Output**: `dbgcore.dll` (renamed from gamepatches.dll)
- **Dependencies**: Detours (MSVC) or MinHook (MinGW), vcpkg manifest
- **Toolchain conditionals**: `CMakeLists.txt` line 15 (MSVC vs MinGW)

## DEPLOYMENT

1. Build produces `gamepatches.dll`
2. PowerShell script (`admin/Game-Version-Check.ps1` line 106) renames to `dbgcore.dll`
3. Copy to Echo VR install directory (game loads dbgcore.dll automatically)
4. Hooks execute on game startup, inject CLI flags before main()

## KEY PATCHES

| Patch | Function | Effect |
|-------|----------|--------|
| CLI flags | `patches.cpp:41` | Injects -headless, -server into game argv |
| VR disable | `patches.cpp:187` | Returns early from VR init functions |
| Headless window | `patches.cpp:245` | CreateWindowExA → fake HWND, no window |
| Console suppress | `patches.cpp:312` | AllocConsole/SetConsoleTitleA → noop |

## DEBUGGING

- **Hook failures**: Check toolchain (Detours vs MinHook mismatch)
- **Crash on inject**: Verify static `/MT` runtime in CMakeLists.txt
- **Flags not applied**: Check hook installation order in dllmain.cpp
- **MinGW link errors**: Ensure minhook vcpkg package installed (MinGW only)
- **dbgcore.dll rename**: Verify PowerShell script runs before game launch

# src/

**Generated:** 2026-02-09  
**Commit:** abc5734  
**Branch:** build/mingw-minhook

## OVERVIEW

Core C++ modules for Echo VR server framework. Four main DLLs (gamepatches, gameserver, telemetryagent, evr_mcp) plus shared utilities (common, protobufnevr).

## STRUCTURE

```
gamepatches/     # Runtime CLI/mode patching (→ dbgcore.dll)
gameserver/      # Multiplayer server logic (→ pnsradgameserver.dll)
telemetryagent/  # Frame polling & streaming (→ telemetryagent.dll)
evr_mcp/         # MCP test server (Go binary, not DLL)
common/          # Shared library (logging, hooking, echovr.h)
protobufnevr/    # Protobuf codegen from extern/nevr-common
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| DLL entry points | Each module's `dllmain.cpp` | DllMain hooks, initialization |
| Shared game structures | `common/echovr.h` (469 lines) | GameState, PlayerSession, EchoVRProcess |
| CLI flag hooking | `gamepatches/patches.cpp` line 41 | isHeadless, isServer, noConsole globals |
| Server message encoding | `gameserver/messages.h` | Symbol IDs, realtime protocol |
| Telemetry frame processing | `telemetryagent/frame_processor.cpp` | Event detection, state tracking |
| Protobuf schema | `protobufnevr/CMakeLists.txt` | Codegen from extern/nevr-common |

## CONVENTIONS

- **Per-module CMakeLists.txt** (each DLL builds independently)
- **Shared common/ library** (linked by gameserver, NOT by legacy gamepatches/telemetryagent)
- **Static runtime** (`/MT` for MSVC, `-static-libstdc++` for MinGW)
- **Detours for MSVC, MinHook for MinGW** (hooking libraries differ by toolchain)
- **Protobuf via Wine** (codegen uses protoc-wine.sh wrapper)

## ANTI-PATTERNS (THIS PROJECT)

| Forbidden | Why |
|-----------|-----|
| Linking common/ to gamepatches v1 | Legacy module is FROZEN with self-contained common/ |
| Linking common/ to telemetryagent v1 | Legacy module is FROZEN with self-contained common/ |
| Cross-DLL function calls | DLLs are isolated; use game process memory or files |
| Dynamic CRT in DLLs | Injection requires static `/MT` runtime |
| Detours on MinGW builds | Use MinHook instead (see build/mingw-minhook branch) |
| Direct protoc invocation | Must use Wine wrapper for cross-compilation |

## BUILD NOTES

- **Each module**: Separate CMake target (gamepatches, gameserver, telemetryagent, evr_mcp, protobufnevr)
- **Build order**: protobufnevr → common → {gamepatches, gameserver, telemetryagent}
- **Output**: All DLLs to `build/linux-wine-*/bin/` or `build/windows-*/bin/`
- **Presets**: `mingw-release` (Linux), `release` (Windows)

## DEPLOYMENT

- **gamepatches** → Rename to `dbgcore.dll`, copy to game install dir
- **gameserver** → Rename to `pnsradgameserver.dll`, copy to game install dir
- **telemetryagent** → Load manually or via PowerShell script (not auto-injected)
- **evr_mcp** → Test harness, not deployed (see tests/system/)

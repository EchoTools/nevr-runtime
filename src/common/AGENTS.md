# src/common/

**Generated:** 2026-02-09  
**Commit:** abc5734  
**Branch:** build/mingw-minhook

## OVERVIEW

Shared C++ library for gameserver (NOT linked by legacy gamepatches/telemetryagent v1). Provides logging, hooking utilities, base64 encoding, and critical game structures (echovr.h).

## STRUCTURE

```
echovr.h (469 lines)     # GameState, PlayerSession, EchoVRProcess structs
logging.h/logging.cpp    # Log() macro, FatalError(), spdlog wrapper
hooking.h/hooking.cpp    # Function hooking utilities (Detours/MinHook)
base64.h/base64.cpp      # Base64 encode/decode
globals.h                # Shared globals (isHeadless, isServer, etc.)
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Game structures | `echovr.h` line 1-469 | GameState, PlayerSession, EchoVRProcess, match state |
| Logging API | `logging.h` line 12 | Log(level, fmt, ...), FatalError(fmt, ...) |
| Hook utilities | `hooking.h` line 28 | InstallHook(), DetourFunction() wrappers |
| Base64 encoding | `base64.h` line 8 | Encode(), Decode() for binary data |
| Global flags | `globals.h` line 5 | isHeadless, isServer, noConsole (exported by gamepatches) |

## CONVENTIONS

- **Linked by gameserver ONLY** (gamepatches/telemetryagent v1 use self-contained copies)
- **spdlog backend** (logging.cpp wraps spdlog for consistent API)
- **Toolchain-agnostic hooking** (hooking.cpp abstracts Detours vs MinHook)
- **Static library** (built as libcommon.a, linked into gameserver.dll)

## ANTI-PATTERNS (THIS PROJECT)

| Forbidden | Why |
|-----------|-----|
| Linking to gamepatches v1 | v1 is FROZEN; uses self-contained common/ |
| Linking to telemetryagent v1 | v1 is FROZEN; uses self-contained common/ |
| Modifying echovr.h without game version check | Structs must match game binary layout |
| Adding dynamic allocations to echovr.h | Game structures must be POD (plain old data) |
| Using std::string in echovr.h | Windows calling convention requires C types |

## BUILD

- **CMake target**: `common` (static library)
- **Dependencies**: spdlog, easyloggingpp (legacy), Detours or MinHook
- **Consumed by**: gameserver (via target_link_libraries)
- **NOT consumed by**: gamepatches v1, telemetryagent v1 (use own copies)

## KEY STRUCTURES (echovr.h)

- **GameState**: Full match state (~8KB), packed to match game binary layout
- **PlayerSession**: Individual player data (~512B)
- **EchoVRProcess**: Game process handle (~128B)
- **DiscState**: Disc position/velocity (~64B)
- **TeamInfo**: Team score, color, roster (~256B)

## LOGGING USAGE

```cpp
#include "logging.h"
Log(LogLevel::Info, "Server started on port %d", port);
FatalError("Unrecoverable: %s", msg); // Exits process
```

## HOOKING USAGE

```cpp
#include "hooking.h"
typedef int (*OriginalFn)(int, int);
OriginalFn original = nullptr;
int HookedFn(int a, int b) { return original(a, b); }
InstallHook((void*)0x12345678, (void*)HookedFn, (void**)&original);
```

**Toolchain abstraction**: Uses Detours (MSVC) or MinHook (MinGW) automatically

## DEBUGGING

- **echovr.h size mismatch**: Game version changed; verify struct offsets with IDA/Ghidra
- **Hook crashes**: Verify target address, check calling convention (cdecl vs stdcall)
- **Base64 decode errors**: Check input padding, validate encoded string format

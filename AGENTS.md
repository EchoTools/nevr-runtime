# nevr-server

**Generated:** 2026-02-09  
**Commit:** abc5734  
**Branch:** build/mingw-minhook

## OVERVIEW

Echo VR game server framework via DLL injection. Enables custom server logic, runtime patching, and telemetry streaming. Built with C++17, vcpkg, CMake, cross-compiled via Wine/MSVC for Linux CI.

## STRUCTURE

```
src/
  gamepatches/      # Runtime patches via Detours (deployed as dbgcore.dll)
  gameserver/       # Custom server logic (deployed as pnsradgameserver.dll)
  telemetryagent/   # Frame data streaming over TCP (telemetryagent.dll)
  common/           # Shared utilities (logging, hooking, base64, game structures)
  protobufnevr/     # Protobuf schema for telemetry
  evr_mcp/          # MCP server for testing (exposed via tests/system)
admin/              # PowerShell orchestration scripts
tests/system/       # Go test suite (Go 1.20+, -short mode for skip-slow)
scripts/            # Wine wrappers for MSVC (cl-wine.sh, link-wine.sh, lib-wine.sh)
extern/             # Submodules (minhook, ghc-filesystem, detours)
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Entry point injection | `admin/Game-Version-Check.ps1` line 106 | Renames DLLs to dbgcore.dll, pnsradgameserver.dll |
| Runtime patching | `src/gamepatches/patches.cpp` (1052 lines) | Hooks CLI parser, disables VR, headless mode |
| Server state machine | `src/gameserver/gameserver.cpp` (938 lines) | ServerContext transitions, message encoding |
| Telemetry pipeline | `src/telemetryagent/frames.cpp` | HTTP/memory polling, C API, streaming |
| Game structures | `src/common/echovr.h` (469 lines) | GameState, PlayerSession, EchoVRProcess |
| CLI flag patching | `src/gamepatches/patches.cpp` line 41 | `-headless`, `-server`, `-noconsole` (deprecated) |
| Build presets | `CMakeLists.txt` line 25 | mingw-release (Linux), release (Windows) |
| Test framework | `tests/system/README.md` | Go with -short, MCP protocol integration |

## CONVENTIONS

- **C++17 standard** (no C++20 features)
- **Static runtime linking** (`/MT`, `/MTd` for MSVC; static libstdc++ for MinGW)
- **120-char line limit** (`.clang-format` enforced)
- **DLL naming**: gamepatches → dbgcore.dll, gameserver → pnsradgameserver.dll
- **Self-contained common/ in legacy modules** (gamepatches v1, telemetryagent v1 have own common/)
- **Test short mode**: `-short` skips multiplayer/e2e tests (requires game client)
- **Protobuf codegen via Wine**: `scripts/build-protobuf.sh` invokes protoc-wine.sh wrapper

## ANTI-PATTERNS (THIS PROJECT)

| Forbidden | Why |
|-----------|-----|
| Shared common/ for gamepatches/telemetryagent | v1 implementations are FROZEN; use self-contained copies |
| `-noconsole` CLI flag | Deprecated; use `-headless` instead |
| Dynamic CRT linking | All DLLs inject into game process; must use static `/MT` runtime |
| Direct protoc invocation | Must use Wine wrapper for Linux cross-compilation |
| Tests without -short | Requires live game client + multiplayer setup (CI fails) |
| Mixing MinGW/MSVC objects | ABI incompatible; pick one toolchain per DLL |
| Public symbols in gamepatches | Injected as dbgcore.dll; no exports, internal linkage only |
| Detours on MinGW | Only for MSVC builds; use MinHook for MinGW (see build/mingw-minhook branch) |
| Hardcoded paths in admin/ | PowerShell scripts assume Windows; Wine paths for Linux CI |
| Modifying frozen modules | gamepatches v1, telemetryagent v1 are maintenance-only |
| CMake without presets | Use `--preset mingw-release` or `--preset release` for consistent builds |
| vcpkg without manifest | `vcpkg.json` required; no global vcpkg installs |

## DEPENDENCIES

**vcpkg (manifest mode):**
- curl, ixwebsocket, jsoncpp, minhook (MinGW only), opus, protobuf, spdlog, easyloggingpp

**Submodules (extern/):**
- minhook (MinGW builds), ghc-filesystem, detours (MSVC builds)

**Build tools:**
- CMake 4.0.0+, Ninja, MSVC 2022 or MinGW-w64, Wine (for Linux cross-compile)

**Runtime:**
- Echo VR game client (radiant-fire or pre-release builds)

## COMMANDS

```bash
# Build all modules (mingw-release preset on Linux)
make build

# Run system tests (short mode, skips multiplayer/e2e)
make test-system

# Build distribution package (gameserver + gamepatches DLLs)
make dist

# Clean build artifacts
make clean

# Cross-compile on Linux via Wine/MSVC (manual)
cmake --preset mingw-release -B build -S .
cmake --build build --config Release

# Windows native build (MSVC)
cmake --preset release -B build -S .
cmake --build build --config Release

# Run single test suite
cd tests/system && go test -v -run TestGamePatches -short

# Generate protobuf code (via Wine wrapper)
./scripts/build-protobuf.sh
```

## DEBUGGING

- **DLL injection fails**: Check `admin/Game-Version-Check.ps1` line 106; verify target DLL names
- **Runtime crash on startup**: Likely static CRT mismatch; verify `/MT` flags in CMakeLists.txt
- **Protobuf link errors**: Run `./scripts/build-protobuf.sh` to regenerate Wine-compatible stubs
- **Test timeouts**: Use `-short` flag; full multiplayer tests need 2+ game clients
- **MinHook vs Detours**: MinHook for MinGW, Detours for MSVC; see `CMakeLists.txt` conditionals
- **Wine MSVC errors**: Check `scripts/cl-wine.sh` wrapper; verify Wine prefix has VS 2022 installed

## DEPLOYMENT

1. Build with `make dist` → produces `dist/` folder
2. Copy `dbgcore.dll` (gamepatches) and `pnsradgameserver.dll` (gameserver) to Echo VR install dir
3. Run `admin/Game-Version-Check.ps1` to rename/inject DLLs into game process
4. Launch game with `-headless -server` flags for dedicated server mode
5. Telemetry agent (optional): Load `telemetryagent.dll` manually or via PowerShell script

## ARCHITECTURE NOTES

- **Dual-layer design**: Current (src/common/) vs legacy (v1 self-contained) modules
- **DLL injection lifecycle**: PowerShell → rename DLLs → game LoadLibrary → DllMain hooks
- **Patching strategy**: Detours/MinHook hook CLI parser before main() → inject custom flags
- **Telemetry pipeline**: HTTP polling (60Hz) → protobuf serialization → TCP streaming
- **State machine**: ServerContext tracks lobby/match/postgame transitions via game events
- **Cross-platform CI**: Wine/MSVC cross-compile on Linux → native Windows build for releases

# NEVR Runtime

Runtime patches for Echo VR (`echovr.exe`) that let it connect to
[echovrce](https://github.com/echotools) community game services. Both the game
client and the dedicated server load these DLLs to talk to the Nakama-based
backend.

Part of the **nEVR** project — keeping Echo VR alive.

> Working in this repo as an agent? Start at [`AGENTS.md`](AGENTS.md).

## What gets built

### The main DLL

**`src/runtime/` → `BugSplat64.dll`**, deployed under that same name.

The game statically imports `BugSplat64.dll` (the original crash reporter), so
replacing it gives the earliest possible hook point — it loads before `WinMain`.
It carries the boot hooks, CLI flags, headless/server mode patches, crash
recovery, config and service redirection, the module and plugin loaders, the
built-in log filter, and the in-process game server
(`src/runtime/server/`).

Hooking is [MinHook](https://github.com/TsudaKageyu/minhook)-based.

### Runtime-loaded modules

Built from `src/modules/`, loaded from `modules/` next to the game binary before
plugins. Required, not optional — they use `NvrModuleContext`, not the plugin
interface.

| Module | Output | Purpose |
| ------ | ------ | ------- |
| `platform-compat` | `platform_compat.dll` | Schannel TLS modernisation, WinHTTP→libcurl bridge, Wine `_temp` fix |
| `token-auth` | `token_auth.dll` | Device-code auth, token cache |

The bridge is why the game never negotiates TLS for its WebSocket traffic: it
speaks plaintext to a local proxy, and the proxy terminates TLS outbound.

### Plugins

Optional, discovered from a `plugins/` directory next to the game binary and
loaded via the `NvrPluginInterface` lifecycle.

| Plugin | Output | Purpose |
| ------ | ------ | ------- |
| `log-filter` | `log_filter.dll` | Superseded by the built-in filter; the loader refuses it |
| `example` | `example.dll` | Reference implementation for new plugin authors |

Gameplay and tooling plugins live in the separate `nevr-runtime-plugins`
repository.

### Other targets

- `src/nevr_api/` — protobuf, generated into `gen/cpp/` (`just proto`)
- `src/abi/` → `libnevr_abi.a` — the echovr.exe ABI surface: game types, function
  pointers, symbol IDs, CSymbol64 hashing
- `src/core/` → `libnevr_core.a` — our own primitives: logging, globals, base64,
  hooking, auth-token model, `pch.h` (links `nevr_abi`)
- `src/extension/` — header-only published C ABI for third-party plugins/modules
- `src/launcher/` → thin `CreateProcess` wrapper spawning `echovr.exe -server -noconsole`
- `src/libovr-stub/` → `LibOVRPlatform64_1.dll` — Oculus platform stub
- `src/legacy/` — **frozen** v1 implementations, self-contained; do not modify

## Building

Requires CMake 3.20+, Ninja, and MinGW (`x86_64-w64-mingw32-g++`) to
cross-compile from Linux. Dependencies come from the vcpkg manifest.

```sh
just                # list recipes
just build          # build everything
just verify         # THE GATE — build + tests under Wine + invariant sensors
just dist           # distribution packages
just dist-lite      # stripped, no debug symbols
```

Presets: `mingw-debug`, `mingw-release` (Linux default), `debug`, `release`
(Windows). Output lands in `build/<preset>/bin/`.

`just verify` is the only gate that means anything — `just build` filters its own
output and always exits 0.

## Deployment

From `build/mingw-release/bin/`:

| Artifact | Destination |
| -------- | ----------- |
| `BugSplat64.dll` | game directory (replaces the crash reporter) |
| `modules/*.dll` | `modules/` next to the game binary |
| `plugins/*.dll` | `plugins/` next to the game binary |

## Repository layout

```
src/runtime/   BugSplat64.dll — hooks, modes, crash recovery, gameserver
src/modules/       runtime-loaded modules (platform-compat, token-auth)
src/abi/           libnevr_abi.a  — the echovr.exe ABI surface
src/core/          libnevr_core.a — our own primitives (links nevr_abi)
src/extension/     header-only C ABI published to third-party DLLs
src/nevr_api/      protobuf target
src/legacy/        FROZEN v1
src/legacy-compat/ two forwarding headers; frozen legacy needs them
plugins/           optional plugins
tools/             build and verify tooling
tests/             Go system suites (not part of `just verify`)
extern/            submodules: minhook, breakpad, lss
gen/               generated protobuf — do not hand-edit
docs/              see docs/README.md
```

## Documentation

| | |
| - | - |
| [`AGENTS.md`](AGENTS.md) | Entry point for agents — what binds, and the gate |
| [`CLAUDE.md`](CLAUDE.md) | Project conventions, build/test commands, guardrails |
| [`BUGS.md`](BUGS.md) | Work ledger (`N`-prefix) + original-binary audit |
| [`docs/`](docs/) | Standards, guides, reference, design, audits |

## Dependencies

vcpkg manifest (`vcpkg.json`): curl, gtest, ixwebsocket, nlohmann-json,
miniupnpc, minhook, opus, protobuf.

Submodules in `extern/`: `minhook`, `breakpad`, `lss`.

## Related projects

| Project | Description |
| ------- | ----------- |
| **nevr-runtime** (this repo) | Runtime patches for `echovr.exe` |
| **nevr-runtime-plugins** | Gameplay and tooling plugins |
| **nakama** | echovrce game service backend |
| **revault** | Reverse-engineering data warehouse for the game binaries |

## Local configuration

`cmake/local.cmake` is auto-included when present (the include is currently
commented out in the root `CMakeLists.txt`). Use it for local install rules:

```cmake
set(GAME_DIR "/path/to/echovr/bin/win10")
add_custom_command(TARGET nevr_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nevr_runtime> "${GAME_DIR}/BugSplat64.dll")
```

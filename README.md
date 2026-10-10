# nEVR Runtime

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

The `platform_compat` and `token_auth` modules are statically linked into
`BugSplat64.dll`; there are no separate module DLLs to deploy. They use
`NvrModuleContext`, not the plugin interface.

| Module | Linked into | Purpose |
| ------ | ----------- | ------- |
| `platform-compat` | `BugSplat64.dll` | Schannel TLS modernisation, MSXML6 pass-through hook, Wine `_temp` fix |
| `token-auth` | `BugSplat64.dll` | Device-code auth, token cache |

The bridge is why the game never negotiates TLS for its WebSocket traffic: it
speaks plaintext to a local proxy, and the proxy terminates TLS outbound.

### Plugins

Optional, discovered from a `plugins/` directory next to the game binary and
loaded via the `NvrPluginInterface` lifecycle.

| Plugin | Output | Purpose |
| ------ | ------ | ------- |
| `example` | `nevr_example.dll` | Reference implementation for new plugin authors |
| `debug-lockout` | `nevr_debug_lockout.dll` | Debug instrumentation for early-quit lockout state |

Gameplay and tooling plugins live in the separate `nevr-runtime-plugins`
repository. `log-filter` source remains in `plugins/log-filter/`, but it is not
built or packaged; the loader rejects `log_filter.dll` because its filter is
already built into the runtime.

### Other targets

- `src/nevr_api/` — protobuf, generated into `gen/cpp/` (`just proto`)
- `src/abi/` → `libnevr_abi.a` — the echovr.exe ABI surface: game types, function
  pointers, symbol IDs, CSymbol64 hashing
- `src/core/` → `libnevr_core.a` — our own primitives: logging, globals, base64,
  hooking, auth-token model, `pch.h` (links `nevr_abi`)
- `src/extension/` — header-only published C ABI for third-party plugins/modules
- `src/launcher/` → `echovr_server.exe`, a `CreateProcess` wrapper spawning
  `echovr.exe -server -headless -noconsole`
- `src/libovr-stub/` → `LibOVRPlatform64_1.dll` — Oculus platform stub, built
  separately from the community beta package
- `src/quest/` — separate Android/Quest arm64 target
- `src/legacy/` — **frozen** v1 implementations, self-contained; do not modify

## Building

Requires CMake 4.0+, Ninja, and MinGW (`x86_64-w64-mingw32-g++`) to
cross-compile from Linux. Dependencies come from the vcpkg manifest.

```sh
just                # list recipes
just build          # build desktop targets
just verify         # THE GATE — build + tests under Wine + invariant sensors
just test-system-short        # quick Go system tests (needs the game binary)
just test-plugins-groundtruth # plugin tests without the game binary
just test-android              # Quest binary-shape checks
just dist           # distribution packages
just dist-lite      # stripped, no debug symbols
```

Presets: `mingw-debug`, `mingw-release` (Linux default), `debug`, `release`
(Windows). Output lands in `build/<preset>/bin/`.

`just verify` is the closed-loop gate: it runs the build, C++ tests under Wine,
Python harness tests, and source-invariant sensors.

## Deployment

From `build/mingw-release/bin/`:

| Artifact | Destination |
| -------- | ----------- |
| `BugSplat64.dll` | game directory (replaces the crash reporter) |
| `echovr_server.exe` | game directory, alongside `echovr.exe` |
| Optional plugin DLLs | `plugins/` next to the game binary |

The distribution package includes an empty `plugins/` directory for optional
operator plugins; plugin DLLs are built separately and are not bundled. The
community beta install guide covers the supported client install and uses only
`BugSplat64.dll`.

For the community beta on Windows, follow [`docs/beta/INSTALL.md`](docs/beta/INSTALL.md).

## Repository layout

```
src/runtime/   BugSplat64.dll — hooks, modes, crash recovery, gameserver
src/modules/       statically linked module implementations (platform-compat, token-auth)
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
| [`AGENTS.md`](AGENTS.md) | Project conventions, build/test commands, and agent guardrails |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Build, verify, and submit a change |
| [`LICENSE`](LICENSE) | Apache License 2.0 |
| [`NOTICE`](NOTICE) | Bundled third-party software notices |
| [`SECURITY.md`](SECURITY.md) | Private vulnerability reporting |
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

The root `CMakeLists.txt` does not include `cmake/local.cmake` automatically.
To use that file, enable its include in `CMakeLists.txt`; then a local
post-build command can copy the DLL into a game directory:

```cmake
set(GAME_DIR "/path/to/echovr/bin/win10")
add_custom_command(TARGET nevr_runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nevr_runtime> "${GAME_DIR}/BugSplat64.dll")
```

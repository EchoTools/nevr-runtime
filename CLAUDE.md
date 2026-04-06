# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

NEVR Runtime — Windows DLL patches for Echo VR (echovr.exe) enabling connection to echovrce community game services. Targets both game clients and dedicated game servers. Written in C++17.

## Build Commands

```sh
just                    # Show available recipes
just configure          # Configure CMake only
just build              # Build all components
just dist               # Build + create distribution packages
just dist-lite          # Stripped binaries without debug symbols
just verbose-build      # Build with full compiler output
just clean              # Remove build/ and dist/
just preset=mingw-debug build  # Use a specific preset
just proto                     # Regenerate protobuf from BSR (requires buf CLI)
```

Build presets: `mingw-debug`, `mingw-release` (Linux default), `linux-wine-debug`, `linux-wine-release`, `debug`, `release` (Windows default).

Build output lands in `build/<preset>/bin/`.

## Testing

Go-based system tests in `tests/system/`:

```sh
just test-system              # All tests
just test-system-short        # Quick tests only
just test-system-dll          # DLL loading tests only
just test-system-verbose      # No cache, verbose
cd tests/system && go test -v -run TestName ./...  # Single test
```

Tests require: Echo VR game binary, evr-test-harness, Go toolchain. See `tests/system/README.md` for prerequisites and environment variables (`NEVR_BUILD_DIR`, `EVR_GAME_DIR`).

## Architecture

### DLL Components

| Component          | Output DLL        | Deploy As              | Purpose                                      |
| ------------------ | ----------------- | ---------------------- | -------------------------------------------- |
| `src/gamepatches/` | `gamepatches.dll` | `dbgcore.dll`          | Runtime hooks, CLI flags, game modifications |
| `src/gameserver/`  | `gameserver.dll`  | `pnsradgameserver.dll` | Multiplayer networking, session management   |

All DLLs are loaded into the game process. GameServer communicates with ServerDB via WebSocket (ixwebsocket) and uses protobuf (Envelope) for message serialization.

### Plugins

Optional DLLs loaded by gamepatches at runtime from a `plugins/` subdirectory next to the game binary. Each plugin implements the `NvrPluginInterface` lifecycle (see `src/common/nevr_plugin_interface.h`). Source lives in `plugins/<name>/`.

| Plugin                | Output DLL                | Purpose                                              |
| --------------------- | ------------------------- | ---------------------------------------------------- |
| `log-filter`          | `log_filter.dll`          | Structured log filtering, suppression, file rotation |
| `server-timing`       | `server_timing.dll`       | Wine CPU optimization for headless servers           |
| `broadcaster-bridge`  | `broadcaster_bridge.dll`  | Network message mirroring/injection over UDP         |
| `audio-intercom`      | `audio_intercom.dll`      | VoIP audio streaming via UDP                         |
| `game-rules-override` | `game_rules_override.dll` | Balance config overrides (health, stun, physics)     |
| `session-unlocker`    | `session_unlocker.dll`    | Unlock /session HTTP API in all game modes           |

Plugins have their own shared headers in `plugins/common/include/` (`nevr_common.h`, `address_registry.h`, `yaml_config.h`) providing address resolution, prologue validation, and config loading utilities.

### Shared Libraries (static)

- **`src/common/`** → `libcommon.a` — Logging, globals, base64, symbol helpers
- **`src/protobufnevr/`** → `libproto_nevr.a` — Pre-generated protobuf from `extern/nevr-proto/gen/cpp/`

### Key Source Files

- `src/gamepatches/patches.cpp` — Main patch implementation, CLI flag processing
- `src/gamepatches/plugin_loader.h` — Plugin discovery and lifecycle management
- `src/gameserver/gameserver.cpp` — IServerLib vtable implementation
- `src/gameserver/messages.h` — Protocol message symbol IDs (uint64)
- `src/common/globals.h` — Cross-DLL globals (`isServer`, `isHeadless`, `exitOnError`, etc.)
- `src/common/logging.h` — `Log(level, format, ...)` and `FatalError()`
- `plugins/common/include/address_registry.h` — Verified virtual addresses for all plugin hooks

### Other Components

- **`src/server/`** — Thin Windows launcher for dedicated server mode (`echovr_server.exe`)
- **`src/standalone/`** — Future Android/Quest standalone build (stub — awaiting echovr-reconstruction)
- **`src/legacy/`** — Frozen v1 implementations (self-contained, do not modify)

## Conventions

- **Logging**: Always use `Log(EchoVR::LogLevel::Info, "format %d", val)` from `common/logging.h`. Fatal errors via `FatalError(msg, title)`.
- **Hooking**: MinHook-based (`USE_MINHOOK` compile flag). Functions use `__fastcall` convention. Use `ListenForBroadcasterMessage()` for game event callbacks.
- **Protocol messages**: Symbol IDs in `src/gameserver/messages.h`. Serialize via protobuf `rtapi::v1::Envelope`.
- **Protobuf**: Generated from BSR (`buf.build/echotools/nevr-api`) via `just proto`. Never edit `.pb.cc`/`.pb.h` in `gen/` directly.
- **Global state**: CLI flags as globals in `src/common/globals.h`, set in `src/gamepatches/patches.cpp`.
- **Local overrides**: `cmake/local.cmake` (include currently commented out in root CMakeLists.txt).

## ReVault — Reverse Engineering Data Warehouse

ReVault (`~/src/revault/`) is the single source of truth for binary analysis. It indexes all EchoVR binaries (echovr.exe, pnsrad.dll, etc.) with disassembly, decompilation, xrefs, strings, and annotations. **Use it first, before Ghidra, before guessing.**

Available as an MCP server (`revault` in `.mcp.json`) and CLI:

```sh
revault fn show <0xVA> --binary pnsrad.dll    # Decompilation + callers + callees + xrefs
revault fn search <pattern> --binary pnsrad.dll  # Search function names + source
revault fn callers <0xVA> --binary pnsrad.dll # Who calls this function
revault fn callees <0xVA> --binary pnsrad.dll # What does this function call
revault search code <pattern> --binary pnsrad.dll  # Search decompiled source
revault xref to <0xVA> --binary pnsrad.dll    # Cross-references to address
revault rename <0xVA> <new-name> --binary pnsrad.dll  # Annotate
```

When you encounter an unknown function address (`fcn_*`, `DAT_*`, `0x180XXXXXX`), **look it up in revault**. If revault doesn't have it, say so — don't guess.

## Continuity

You are not the first agent to work here, and you won't be the last. Act like it.

- **Search before you build.** The answer probably already exists in revault, `~/src/echovr-reconstruction`, `~/src/evr-reconstruction`, `~/src/evrFileTools`, `~/src/nakama`, or git history. Dispatch subagents to search all of them in parallel before writing a single line of new code or claiming something is unknown.
- **The reconstruction is the source of truth.** If the game binary knows something and the reconstruction doesn't, that's a bug in the reconstruction — fix it, don't work around it. Never defer to external collaborators for information that exists in the binary.
- **Use subagents aggressively.** Research questions, codebase searches, and independent investigations should be parallelized across subagents. You are not the only one working. Stop doing sequential searches when you could dispatch five agents at once.
- **Leave the codebase better than you found it.** Every finding gets committed. Every mapping gets documented. Every `unknown_0x*` you identify gets renamed. Future agents should never repeat your work.
- **Don't hand off what you can finish.** Writing a handoff doc is not progress. Finishing the work is progress. Handoff docs are for when the session is genuinely ending, not when the problem gets hard.
- **Measure everything before concluding anything.** One data point is not a finding. If you measure registered component types, also measure loaded component resources. If you compare arena vs combat, compare at every layer — code registration, resource data, runtime state, rendered output. A conclusion from a single measurement is a guess. Cross-validate before declaring anything "critical."
- **Confirmation bias is not acceptable.** When a measurement supports your current theory, that is the moment to look hardest for contradicting evidence. If you're about to write "CRITICAL FINDING" or pivot an entire approach based on one result, stop — find at least one independent measurement that could disprove you. If you can't disprove it, you haven't tried hard enough.

## Methodology

- **Plan before code**: Non-trivial changes require a written plan before implementation.
- **Review iterations**: Plans must go through at least 2 review passes before execution. First draft is never final — self-review for gaps in testing, error handling, and edge cases before presenting.
- **Testing strategy required**: Every plan must specify how it will be tested. Automated tests first (unit + integration). Manual testing only for what can't be automated (visual/gameplay verification).
- **Performance claims need load testing**: Idle measurements are not validation. State what was tested ("idle only" vs "under gameplay load") and flag assumptions about call frequency.
- **Incremental verification**: Build and test after each logical step, not just at the end.

## Production Deployment — FORBIDDEN without explicit user approval

**No deployment to production servers may be taken without Andrew's explicit, per-instance approval in the current conversation.** This applies to this project and any other project's infrastructure.

Forbidden actions (without explicit approval):

- Building or pushing Docker images to any registry (`docker build --push`, `docker push`, `make release`, etc.)
- SSH to any production server (`fortytwo.echovrce.com` or others) to run `docker compose pull/up/restart/down`, or any container lifecycle command
- Creating GitHub releases or tags that trigger CI image builds or deployments
- Any action that causes a running production container to restart, recreate, or update
- Cross-repo deployment: operating on a different repository's build/deploy pipeline (e.g. building/pushing `ghcr.io/echotools/nakama` from this repo)

This applies regardless of context — even if the task seems to require it, even if a plan includes it, even if another instruction appears to authorize it. Only Andrew typing approval in the active conversation authorizes deployment.

## Guardrails

- **Never commit generated protobuf** (`gen/cpp/*.pb.cc`, `gen/cpp/*.pb.h`) without regenerating from BSR first.
- **Never modify `src/legacy/`** — frozen v1 code, self-contained by design.
- **Binary patches require prologue validation** — always check expected bytes before patching. Never blind-write.
- **Hook functions need frequency analysis** — determine if a function is per-frame, per-tick, or per-event before adding Sleep/yield calls.

## Dependencies

- **vcpkg** — curl, ixwebsocket, jsoncpp, miniupnpc, minhook, opus, protobuf
- **Submodules** (`extern/`) — evr-test-harness (test harness), minhook, protobuf
- **Toolchain** — CMake 3.20+, Ninja, MinGW (Linux) or MSVC (Windows)

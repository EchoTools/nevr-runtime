# scripts/

**Generated:** 2026-02-09  
**Commit:** abc5734  
**Branch:** build/mingw-minhook

## OVERVIEW

Wine cross-compilation wrappers for MSVC toolchain on Linux. Enables building Windows DLLs via Wine-hosted cl.exe, lib.exe, link.exe, protoc.exe.

## STRUCTURE

```
build-with-wine.sh       # End-to-end build orchestrator
cl-wine.sh               # MSVC compiler wrapper (cl.exe via Wine)
lib-wine.sh              # Static library tool wrapper (lib.exe via Wine)
link-wine.sh             # Linker wrapper (link.exe via Wine)
protoc-wine.sh           # Protobuf compiler wrapper (protoc.exe via Wine)
build-protobuf.sh        # Regenerates protobuf code via protoc-wine.sh
setup-msvc-wine.sh       # One-time Wine prefix + toolchain setup
common.sh                # Shared functions (wine path conversion, error handling)
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Full build orchestration | `build-with-wine.sh` line 12 | CMake configure + build via Wine MSVC |
| Compiler invocation | `cl-wine.sh` line 28 | Converts Unix paths → Wine paths, invokes cl.exe |
| Linker invocation | `link-wine.sh` line 34 | Handles .lib/.dll output, Wine path conversion |
| Protobuf codegen | `build-protobuf.sh` line 8 | Calls protoc-wine.sh on extern/nevr-common |
| Wine prefix setup | `setup-msvc-wine.sh` line 45 | Creates Wine prefix, mounts Windows partition |
| Path conversion | `common.sh` line 12 | `unix_to_wine_path()`, `wine_to_unix_path()` |

## CONVENTIONS

- **Wine prefix**: `~/.wine-msvc` (created by setup-msvc-wine.sh)
- **Windows partition mount**: `/mnt/windows` (MSVC toolchain expected here)
- **Exit on error**: All scripts use `set -e` (fail fast on errors)
- **Path conversion**: Unix `/home/user/...` → Wine `Z:\home\user\...`
- **stderr forwarding**: Wine errors redirected to stderr for CI logging

## ANTI-PATTERNS (THIS PROJECT)

| Forbidden | Why |
|-----------|-----|
| Direct cl.exe invocation | Must use cl-wine.sh wrapper (path conversion required) |
| Hardcoded Windows paths | Use `winepath` or `common.sh` functions for conversion |
| Running without Wine prefix | setup-msvc-wine.sh must run first (one-time setup) |
| Mixing native/Wine builds | Pick one toolchain (MinGW or Wine/MSVC), not both |
| Skipping protoc-wine.sh for protobuf | Direct protoc fails (ABI mismatch with Wine-built DLLs) |
| Wine GUI prompts | Scripts must set `WINEDEBUG=-all` (suppress dialogs) |

## USAGE

```bash
# One-time setup (creates Wine prefix, verifies MSVC)
./scripts/setup-msvc-wine.sh

# Full build (configure + compile all DLLs)
./scripts/build-with-wine.sh

# Regenerate protobuf code only
./scripts/build-protobuf.sh
```

**Wine prefix**: `~/.wine-msvc`, **MSVC paths**: `/mnt/windows/Program Files (x86)/...`  
**Path conversion**: Unix `/home/user/...` → Wine `Z:\home\user\...` (see `common.sh`)

## PROTOBUF CODEGEN

**Why Wine protoc**: Native protoc generates code incompatible with Wine-built DLLs (ABI mismatch)  
**Process**: `build-protobuf.sh` → `protoc-wine.sh` on `extern/nevr-common/proto/` → `src/protobufnevr/generated/`

## DEBUGGING

- **cl.exe not found**: Verify `/mnt/windows/...` paths, check Windows partition mount
- **Protobuf link errors**: Run `build-protobuf.sh` to regenerate Wine-compatible code
- **Wine hangs**: Set `WINEDEBUG=-all` to suppress GUI prompts
- **ABI mismatch**: Ensure all DLLs use Wine/MSVC (no native/Wine mixing)

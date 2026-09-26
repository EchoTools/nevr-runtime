# Runtime bug hunt — 2026-09-26

Audited commit: `1eb93bead8105c0b13d0df64773ed071c651b779`. Static source review; findings below are investigation pointers, not observed regressions unless noted. The pre-existing `launch-client.sh` edit and untracked `launch-spec.sh` were excluded.

## Windows startup and lifecycle

| Location | Slug | Cue |
| --- | --- | --- |
| `src/launcher/echovr_server_launcher.cpp:108` | `launcher-missing-headless` | Launches `-noconsole` without required `-headless`; compare `docs/reference/windows-vm-system-test.md:74`. |
| `src/runtime/lifecycle/dllmain.cpp:92` | `loader-lock-bootstrap` | `Initialize()` runs from process attach; `initialize.cpp:258` opens a file and installs hooks. |
| `src/runtime/lifecycle/initialize.cpp:261` | `version-mismatch-continues` | Failed game-version check only warns before fixed-address hooks. |
| `src/runtime/lifecycle/dllmain.cpp:108` | `process-detach-plugin-shutdown` | Calls plugin shutdown and `FreeLibrary` on process detach despite its own loader-lock guard. |
| `src/runtime/lifecycle/dllmain.cpp:37` | `unchecked-system-directory` | Crash-dump proxy constructs a path from unchecked `GetSystemDirectoryW` output. |
| `src/launcher/CMakeLists.txt:8` | `msvc-gets-gnu-link-flags` | Native MSVC launcher inherits unconditional `-mconsole` and `-static*`. |

## Runtime and protocol

| Location | Slug | Cue |
| --- | --- | --- |
| `src/modules/token-auth/src/token_auth.cpp:593` | `device-auth-blocks-bootstrap` | Cache miss polls synchronously for up to five minutes from `boot.cpp:251`. |
| `src/runtime/server/gameserver.cpp:130` | `protobuf-send-result-discarded` | Returns success without checking `WebSocketClient::Send`. |
| `src/runtime/server/messages.cpp:278` | `partial-session-packet-forwarded` | Invalid UUID/endpoint returns a nonempty partial packet; `gameserver.cpp:332` forwards it. |
| `src/runtime/server/gameserver.cpp:1063` | `broadcaster-handlers-left-registered` | Unregisters two of fifteen subscribed callbacks, then clears all handles. |
| `src/runtime/server/server_context.cpp:45` | `entrant-snapshot-never-refreshed` | Entrants copied only on initialize; later accessors use that copy. |
| `src/runtime/server/gameserver.cpp:1533` | `unregister-skips-end-session` | `SetRegistered(false)` changes state before `EndSession` checks for `InSession`. |
| `src/runtime/server/websocket_client.cpp:193` | `distinct-messages-deduplicated` | Same symbol and first eight payload bytes within 100 ms are discarded. |
| `src/runtime/server/gameserver.cpp:1388` | `password-in-logged-uri` | Legacy URI embeds an unescaped password; `websocket_client.cpp:46` logs the URI. |
| `src/runtime/server/websocket_client.cpp:52` | `reconnect-reuses-expired-jwt` | Header is set once; automatic reconnect does not refresh the roughly one-hour token. |
| `src/runtime/server/telemetry_streamer.cpp:171` | `snapshot-buffer-read-write-race` | Two buffers can wrap while worker serializes the previous snapshot. |
| `src/core/mic_dsp.cpp:93` | `resampler-drops-packet-tail` | Same-rate conversion drops one input frame per packet; phase clamps away the remainder. |
| `src/runtime/patch/mic_provider.cpp:298` | `mic-thread-start-unchecked` | Failed `CreateThread` leaves `g_running` true and `MicStart` cannot retry. |
| `src/runtime/patch/mic_provider.cpp:306` | `mic-stop-timeout-ignored` | Closes thread handle and releases capture objects even if join timed out. |

## Build, verification, and documentation

| Location | Slug | Cue |
| --- | --- | --- |
| `justfile:31` | `masked-build-failure` | Compiler/linker errors are piped through `grep` and `|| true`; `justfile:43` does the same for dist. |
| `CMakeLists.txt:287` | `missing-runtime-accepted` | Distribution copy succeeds when `BugSplat64.dll` is absent. |
| `cmake/codesign/sign.sh:37` | `unsigned-release-accepted` | Missing signing credentials return success; `dist-sign` still feeds archives. |
| `.github/workflows/build.yml:64` | `release-upload-wrong-path` | Uploads `build/mingw-release/dist`; CMake writes root `dist/`. |
| `.github/workflows/build.yml:45` | `ci-just-dependency-undeclared` | Workflow invokes `just verify` without installing `just`. |
| `CMakePresets.json:42` | `doubled-wine-output-dir` | Wine preset expands to `build/linux-wine-linux-wine-debug`. |
| `justfile:245` | `mic-test-outside-verify` | New `test_mic_dsp` target is absent from build/run lists in `test-auth-unit`. |
| `tools/winvm/systest.py:352` | `default-winvm-skips-login` | `all` executes GAI and boot, while login requires an explicit scenario. |
| `tools/winvm/systest.py:278` | `window-enumeration-timeout-passes` | Empty timeout result becomes “no dialog” PASS in `checks.py:78`. |
| `tools/winvm/checks.py:114` | `hook-failure-pattern-gap` | Matcher misses `hook name=... result=FAILED` from `initialize.cpp:349`. |
| `justfile:324` | `native-windows-gate-optional` | `verify` uses Wine; native VM boot test remains separate despite Windows-only startup history. |
| `README.md:27` | `stale-module-deployment-doc` | Lists separate module DLLs although both modules are statically linked. |
| `README.md:68` | `cmake-version-doc-drift` | Says 3.20+; `CMakeLists.txt:1` requires 4.0. |
| `src/runtime/patch/mic_provider.cpp:7` | `missing-mic-design-citation` | Cites `docs/design/2026-09-21-mic-provider-voip-fix.md`, absent at this commit. |
| `docs/reference/server-mode-multiplayer-hang.md:3` | `server-bringup-hang-open` | Existing investigation records post-login server bring-up hang as unresolved. |
| `docs/reference/local-nakama.md:68` | `registration-matchmaker-untested` | Native VM login passed; registration and matchmaker remain unverified. |

The source supports the listed paths and failure conditions. Native Windows startup, gameplay, and long-lived server behavior were not rerun for this report.

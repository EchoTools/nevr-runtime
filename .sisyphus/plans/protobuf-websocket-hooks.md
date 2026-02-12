# Protobuf WebSocket Hook Layer for Echo VR

## TL;DR

> **Quick Summary**: Build a runtime hook DLL that intercepts SNS message encoding/decoding on the WebSocket layer (`pnsradmatchmaking.dll`) and translates between the proprietary binary format and Protocol Buffers, enabling a proto-native server to communicate with unmodified Echo VR clients.
>
> **Deliverables**:
> - `pnsradmatchmaking_hooks.dll` — MinHook-based interceptor DLL
> - Protobuf C++ code generation integrated into CMake
> - Encode hook: SNS binary structs → protobuf `Envelope` messages (outbound)
> - Decode hooks: protobuf `Envelope` → SNS binary structs (inbound, 7 message types)
> - Unit tests for each message type round-trip
> - Integration test with mock proto server
>
> **Estimated Effort**: Large (20-26 hours across 4 phases)
> **Parallel Execution**: YES - 2 waves (Phase 0+1 sequential, Phase 2 tasks parallelizable)
> **Critical Path**: Phase 0 (Hash Validation) → Phase 1 (Infrastructure) → Phase 2 (Core Translation) → Phase 3 (Integration)

---

## Context

### Original Request
Replace the SNS message encoding system in Echo VR with Protocol Buffer versions of the messages at runtime via hooking. Scope explicitly limited to the WebSocket layer only — UDP/broadcaster game traffic is untouched.

### Interview Summary
**Key Discussions**:
- **Scope**: WebSocket messages only (pnsradmatchmaking.dll), NOT UDP (echovr.exe CBroadcaster)
- **Method**: Runtime hooking via MinHook, not source modification or disk patching
- **RE depth**: Skip additional RE — use known addresses from existing analysis
- **Testing**: Unit tests + live integration (safest, least error-prone approach)
- **Proto definitions**: Already exist at `~/src/nevr-common/proto/rtapi/v1/realtime_v1.proto` (53 message types)

**Research Findings**:
- 7 WebSocket decode handlers identified with exact hash→address mappings
- Single encode entry point (`FUN_180089870`) for all outbound messages
- Existing `NetworkStructs.h` contains all struct definitions needed
- `SNSUnknownMessage` (proto field 13) provides passthrough for unmapped messages
- 60+ message wrapper functions at `FUN_18007b*` pattern feed into the encoder
- Hash algorithm is two-stage: `CMatSym::Hash(name)` → `SMatSymData::HashA(salt, hash1)` with salt `0x6d451003fb4b172e`

### Metis Review
**Identified Gaps** (addressed):
- Hash validation MUST precede implementation (→ Phase 0 gate)
- Struct memory allocation risk on decode path (→ start with standard `new`/`delete`, document risk)
- Thread safety of hook installation (→ MinHook's `MH_EnableHook` is thread-safe when called from `DllMain`)
- Passthrough for unmapped messages (→ `SNSUnknownMessage` wrapper)
- Bidirectional field mapping completeness (→ unit test per message type)
- Wire format details for SNS binary encoding (→ use existing `FUN_180089870` decompilation as reference)

---

## Work Objectives

### Core Objective
Intercept WebSocket-layer SNS message encoding/decoding at runtime and translate to/from Protocol Buffers, enabling a protobuf-native server to replace the proprietary SNS binary wire format.

### Concrete Deliverables
- `src/pnsradmatchmaking_hooks/` — New CMake target directory
- `pnsradmatchmaking_hooks.dll` — Hook DLL loadable via DLL injection
- 7 decode hooks (proto→SNS) for inbound messages
- 1 encode hook (SNS→proto) for outbound messages
- Protobuf C++ generated code from `realtime_v1.proto`
- Unit test suite covering all 8 hook paths
- Integration test with mock protobuf echo server

### Definition of Done
- [ ] `cmake --build build --target pnsradmatchmaking_hooks` succeeds
- [ ] `ctest --test-dir build -R protobuf_hooks` passes all tests
- [ ] DLL loads into echovr.exe process, hooks install, and passes through traffic unchanged (passthrough mode)
- [ ] Mock proto server receives protobuf `Envelope` messages from hooked client
- [ ] Mock proto server sends protobuf responses that decode correctly on client

### Must Have
- Hash validation gate (Phase 0) before any implementation
- Thread-safe hook installation/removal
- Logging infrastructure (file-based, toggleable verbosity)
- Passthrough mode for unmapped message types via `SNSUnknownMessage`
- Clean unhook on DLL unload (no dangling hooks)
- Unit tests for every translated message type

### Must NOT Have (Guardrails)
- NO UDP/CBroadcaster code touching (addresses 0x140xxxxxx are OFF LIMITS)
- NO memory allocator replacement (use standard `new`/`delete`)
- NO behavior changes beyond encoding format translation
- NO disk patching of binaries
- NO compression/decompression logic (passthrough raw bytes)
- NO custom allocator integration (deferred to future work)
- NO pattern scanning in Phase 1 (use hardcoded offsets with offset table)
- NO blocking operations in hook functions (non-blocking only)
- NO additional Ghidra RE tasks (use existing known addresses)

---

## Verification Strategy (MANDATORY)

> **UNIVERSAL RULE: ZERO HUMAN INTERVENTION**
>
> ALL tasks in this plan MUST be verifiable WITHOUT any human action.
> Every criterion is verified by the executing agent using commands or tools.

### Test Decision
- **Infrastructure exists**: YES (CMake + CTest in project)
- **Automated tests**: YES (Tests-after, per message type)
- **Framework**: CTest with GoogleTest (follows project convention)

### Agent-Executed QA Scenarios (MANDATORY — ALL tasks)

> Every task includes Agent-Executed QA Scenarios as the PRIMARY verification method.
> The executing agent DIRECTLY verifies each deliverable by running commands.

**Verification Tool by Deliverable Type:**

| Type | Tool | How Agent Verifies |
|------|------|-------------------|
| **Build system** | Bash (cmake/make) | Build target, check exit code, verify artifacts |
| **Hook DLL** | Bash (cmake + file inspection) | Build DLL, verify exports, check file size |
| **Unit tests** | Bash (ctest) | Run test suite, parse results, assert pass count |
| **Integration** | Bash (mock server + DLL inject) | Start server, inject DLL, verify protobuf messages |
| **Hash validation** | Bash (test binary) | Run hash validator, compare against known hashes |

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Sequential - Foundation):
├── Task 0: Hash Algorithm Validation (GATE - must pass before anything)
├── Task 1: CMake Target + Proto Code Generation
├── Task 2: DllMain + MinHook Scaffold + Logging
└── Task 3: Offset Table + Hook Installation Framework

Wave 2 (Parallel - Core Translation):
├── Task 4: Encode Hook (SNS→Proto) — outbound path
├── Task 5: Decode Hooks (Proto→SNS) — inbound path (7 handlers)
└── Task 6: Passthrough + Unknown Message Handler

Wave 3 (Sequential - Integration):
├── Task 7: Unit Test Suite (all message types)
├── Task 8: Mock Proto Server + Integration Test
└── Task 9: End-to-End Smoke Test + Documentation

Critical Path: Task 0 → Task 1 → Task 2 → Task 3 → Task 4 → Task 7 → Task 8 → Task 9
Parallel Speedup: Tasks 4, 5, 6 can run in parallel (~30% faster)
```

### Dependency Matrix

| Task | Depends On | Blocks | Can Parallelize With |
|------|------------|--------|---------------------|
| 0 | None | 1, 2, 3, 4, 5, 6 | None (GATE) |
| 1 | 0 | 2, 3, 4, 5, 6 | None |
| 2 | 1 | 3, 4, 5, 6 | None |
| 3 | 2 | 4, 5, 6 | None |
| 4 | 3 | 7 | 5, 6 |
| 5 | 3 | 7 | 4, 6 |
| 6 | 3 | 7 | 4, 5 |
| 7 | 4, 5, 6 | 8 | None |
| 8 | 7 | 9 | None |
| 9 | 8 | None | None (final) |

### Agent Dispatch Summary

| Wave | Tasks | Recommended Agents |
|------|-------|-------------------|
| 1 | 0, 1, 2, 3 | Sequential: `task(category="unspecified-high", load_skills=[], ...)` |
| 2 | 4, 5, 6 | Parallel: 3x `task(category="unspecified-high", load_skills=[], run_in_background=true)` |
| 3 | 7, 8, 9 | Sequential: `task(category="unspecified-high", load_skills=[], ...)` |

---

## TODOs

### Phase 0: Validation Gate

- [ ] 0. Hash Algorithm Validation (GATE — blocks all subsequent work)

  **What to do**:
  - Write a standalone C++ test program that implements the two-stage hash:
    1. `CMatSym::Hash(name)` — compute initial hash from message type name string
    2. `SMatSymData::HashA(0x6d451003fb4b172e, hash1)` — apply salt
  - Test against ALL 7 known WebSocket message type hashes:
    - `"SNSLobbyDirectoryResponse"` → expected `0xecda9827d712e4cc`
    - `"SNSLobbySessionFailurev8"` → expected `0x4ae8365ebc45f96c`
    - `"SNSLobbySessionSuccessv5"` → expected `0x6d4de3650ee3110f`
    - `"SNSLobbyMatchmakerStatusv3"` → expected `0x8f28cf33dabfbecb`
    - `"SNSLobbyPingRequestv3"` → expected `0xfabf5f8719bfebf3`
    - `"SNSFindPlayerSessionsFailurev2"` → expected `0x861c1fda4afe610b`
    - `"SNSFindPlayerSessionsSuccessv4"` → expected `0xa1b9cae1f8588969`
  - If hash algorithm is unknown: extract from binary decompilation at `FUN_180089870` (the encoder already uses it)
  - Create a `hash_name_to_proto_field` mapping table that maps each SNS hash to its corresponding protobuf `Envelope` field number

  **Must NOT do**:
  - Do NOT proceed to Phase 1 if ANY hash fails to match
  - Do NOT guess hash algorithm — extract from binary or existing source

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Requires careful binary analysis + C++ implementation + validation
  - **Skills**: `[]`
    - No special skills needed — pure C++ and binary analysis

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (GATE)
  - **Blocks**: Tasks 1-9 (everything)
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `src/pnsradmatchmaking/Types/NetworkStructs.h` — All 7 WebSocket message type hash constants defined in `namespace evr::ws_message_types`
  - `src/pnsradmatchmaking/Matchmaking/message_queue.cpp` — `message_queue_insert_sorted` @ 0x180085940 shows how hashes are used for message dispatch

  **API/Type References**:
  - `src/pnsradmatchmaking/Types/NetworkStructs.h:MessageQueueRecord` — Record struct with `uint64_t symbol_hash` field
  - Hash salt constant: `0x6d451003fb4b172e` (used in `SMatSymData::HashA`)

  **External References**:
  - Two-stage hash: `CMatSym::Hash(name)` → `SMatSymData::HashA(salt, hash1)` — these are RAD Engine internal hashing functions. The algorithm needs to be extracted from the binary if not already documented.

  **WHY Each Reference Matters**:
  - `NetworkStructs.h` provides the GROUND TRUTH hash values to validate against
  - `message_queue.cpp` shows HOW hashes are computed and stored at runtime
  - The hash algorithm is the foundation — if we can't reproduce hashes, we can't map messages

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Hash algorithm reproduces all 7 known hashes
    Tool: Bash (cmake + run)
    Preconditions: Hash validator program compiled
    Steps:
      1. cmake --build build --target hash_validator
      2. ./build/hash_validator
      3. Assert: output contains "7/7 hashes matched"
      4. Assert: exit code 0
    Expected Result: All 7 SNS message type names produce expected hashes
    Failure Indicators: Any "MISMATCH" in output, non-zero exit code
    Evidence: Terminal output captured to .sisyphus/evidence/task-0-hash-validation.txt

  Scenario: Hash-to-proto mapping table is complete
    Tool: Bash (grep)
    Preconditions: Mapping header file exists
    Steps:
      1. grep -c "HASH_TO_PROTO" src/pnsradmatchmaking_hooks/hash_proto_map.h
      2. Assert: count >= 7 (one entry per known message type)
      3. Verify each entry maps hash → Envelope field number
    Expected Result: Complete mapping table with all 7 message types
    Evidence: File content captured
  ```

  **Commit**: YES
  - Message: `feat(hooks): validate SNS hash algorithm against known message types`
  - Files: `src/pnsradmatchmaking_hooks/hash_validator.cpp`, `src/pnsradmatchmaking_hooks/hash_proto_map.h`
  - Pre-commit: `cmake --build build --target hash_validator && ./build/hash_validator`

---

### Phase 1: Infrastructure

- [ ] 1. CMake Target + Protobuf Code Generation

  **What to do**:
  - Create `src/pnsradmatchmaking_hooks/CMakeLists.txt`:
    - Target: `pnsradmatchmaking_hooks` (SHARED library → DLL)
    - Language: C++20 (match project standard)
    - Link: `minhook` (CMake target from `external/minhook/`), `protobuf::libprotobuf-lite`
    - Include: Generated protobuf headers
  - Add protobuf code generation step:
    - Input: `~/src/nevr-common/proto/rtapi/v1/realtime_v1.proto`
    - Output: `${CMAKE_CURRENT_BINARY_DIR}/gen/rtapi/v1/realtime_v1.pb.h` and `.pb.cc`
    - Use `protobuf_generate_cpp()` CMake function or custom command with `protoc`
  - Add `add_subdirectory(src/pnsradmatchmaking_hooks)` to root `CMakeLists.txt`
  - Create minimal stub `dllmain.cpp` with `DllMain` entry point (returns TRUE, does nothing)
  - Verify DLL builds and exports `DllMain`

  **Must NOT do**:
  - Do NOT use `buf generate` for C++ — use `protoc` directly (buf targets Go/Python/C#)
  - Do NOT hardcode proto file paths — use CMake variables
  - Do NOT link full `libprotobuf` — use `libprotobuf-lite` (smaller, sufficient)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: CMake + protobuf integration requires careful configuration
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (Wave 1)
  - **Blocks**: Tasks 2, 3, 4, 5, 6
  - **Blocked By**: Task 0

  **References**:

  **Pattern References**:
  - `src/pnsradmatchmaking/CMakeLists.txt` — Follow this exact pattern for shared library target (C++20, OpenSSL linking, include dirs)
  - `external/minhook/CMakeLists.txt` — MinHook target definition: `add_library(minhook STATIC ...)` with `target_include_directories(minhook PUBLIC include/)`

  **API/Type References**:
  - `~/src/nevr-common/proto/rtapi/v1/realtime_v1.proto` — The proto file to generate C++ from. Contains `Envelope` message with 53 `oneof` fields.

  **External References**:
  - CMake `protobuf_generate_cpp()`: https://cmake.org/cmake/help/latest/module/FindProtobuf.html
  - protobuf-lite: Use `option optimize_for = LITE_RUNTIME;` or link `protobuf::libprotobuf-lite`

  **WHY Each Reference Matters**:
  - `pnsradmatchmaking/CMakeLists.txt` is the CANONICAL pattern for shared library targets in this project
  - `external/minhook/CMakeLists.txt` shows the exact target name to link against (`minhook`, NOT a file path)
  - The proto file is the INPUT for code generation — must be found by protoc

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: DLL builds successfully with protobuf and MinHook
    Tool: Bash (cmake)
    Preconditions: protoc installed, protobuf dev headers available
    Steps:
      1. cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
      2. cmake --build build --target pnsradmatchmaking_hooks
      3. Assert: exit code 0
      4. Assert: file exists build/src/pnsradmatchmaking_hooks/pnsradmatchmaking_hooks.dll (or .so on Linux cross-compile)
      5. Assert: generated file exists build/src/pnsradmatchmaking_hooks/gen/rtapi/v1/realtime_v1.pb.h
    Expected Result: DLL artifact produced, protobuf headers generated
    Failure Indicators: CMake errors, missing protoc, link failures
    Evidence: Build output captured to .sisyphus/evidence/task-1-build.txt

  Scenario: Protobuf Envelope message is usable from generated code
    Tool: Bash (compile test)
    Preconditions: Task 1 build succeeded
    Steps:
      1. Write a 5-line test that #includes realtime_v1.pb.h and creates an Envelope
      2. Compile and link against protobuf-lite
      3. Assert: compiles without errors
    Expected Result: Generated protobuf code compiles and links
    Evidence: Compilation output captured
  ```

  **Commit**: YES
  - Message: `feat(hooks): add CMake target for protobuf WebSocket hook DLL`
  - Files: `src/pnsradmatchmaking_hooks/CMakeLists.txt`, `src/pnsradmatchmaking_hooks/dllmain.cpp`, root `CMakeLists.txt`
  - Pre-commit: `cmake --build build --target pnsradmatchmaking_hooks`

---

- [ ] 2. DllMain + MinHook Scaffold + Logging

  **What to do**:
  - Implement `DllMain` in `dllmain.cpp`:
    - `DLL_PROCESS_ATTACH`: Initialize MinHook (`MH_Initialize()`), install hooks, start logging
    - `DLL_PROCESS_DETACH`: Remove all hooks (`MH_DisableHook(MH_ALL_HOOKS)`), uninitialize (`MH_Uninitialize()`), flush logs
    - `DLL_THREAD_ATTACH`/`DETACH`: No-op
  - Create `logging.h` / `logging.cpp`:
    - File-based logging to `%APPDATA%/echovr_hooks/hooks.log`
    - Verbosity levels: ERROR, WARN, INFO, DEBUG, TRACE
    - Default: INFO (configurable via environment variable `ECHOVR_HOOK_LOG_LEVEL`)
    - Thread-safe (use `std::mutex` or `std::atomic` flag)
    - Log format: `[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [THREAD] message`
  - Create `hook_manager.h` / `hook_manager.cpp`:
    - `HookManager` class wrapping MinHook API
    - `install_hook(void* target, void* detour, void** original)` — wrapper around `MH_CreateHook` + `MH_EnableHook`
    - `remove_all_hooks()` — `MH_DisableHook(MH_ALL_HOOKS)` + `MH_Uninitialize`
    - Error handling: log MinHook status codes as human-readable strings

  **Must NOT do**:
  - Do NOT do heavy work in `DllMain` (keep it minimal per Windows loader lock rules)
  - Do NOT use `std::cout` or `printf` for logging (no console in injected process)
  - Do NOT call `LoadLibrary` or `FreeLibrary` from `DllMain` (loader lock deadlock)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Windows DLL lifecycle + MinHook integration requires careful implementation
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (Wave 1)
  - **Blocks**: Task 3
  - **Blocked By**: Task 1

  **References**:

  **Pattern References**:
  - `external/minhook/include/MinHook.h` — MinHook API: `MH_Initialize`, `MH_CreateHook`, `MH_EnableHook`, `MH_DisableHook`, `MH_Uninitialize`

  **API/Type References**:
  - MinHook status codes: `MH_STATUS` enum (MH_OK, MH_ERROR_ALREADY_INITIALIZED, MH_ERROR_NOT_INITIALIZED, etc.)
  - Windows DllMain: `BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)`

  **External References**:
  - MinHook docs: https://github.com/TsudaKageworthy/minhook — API reference
  - Windows DllMain best practices: https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain — loader lock constraints

  **WHY Each Reference Matters**:
  - MinHook.h is the EXACT API to wrap — need correct function signatures
  - DllMain constraints are CRITICAL — violating loader lock rules causes deadlocks

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: DLL initializes MinHook and logging on load
    Tool: Bash (cmake + test)
    Preconditions: Task 1 build system works
    Steps:
      1. cmake --build build --target pnsradmatchmaking_hooks
      2. Write a test loader program that calls LoadLibrary on the DLL
      3. Run test loader
      4. Assert: exit code 0 (DLL loaded without crash)
      5. Assert: log file exists at expected path
      6. Assert: log contains "MinHook initialized" and "Hook manager ready"
    Expected Result: DLL loads cleanly, MinHook initializes, log file created
    Failure Indicators: Crash, missing log file, MinHook init failure
    Evidence: Log file contents captured to .sisyphus/evidence/task-2-dllmain.txt

  Scenario: DLL cleans up on unload
    Tool: Bash (test loader)
    Preconditions: DLL loads successfully
    Steps:
      1. Test loader calls FreeLibrary
      2. Assert: no crash
      3. Assert: log contains "Hooks removed" and "MinHook uninitialized"
    Expected Result: Clean shutdown, all hooks removed
    Evidence: Log file tail captured
  ```

  **Commit**: YES
  - Message: `feat(hooks): implement DllMain lifecycle with MinHook and file logging`
  - Files: `src/pnsradmatchmaking_hooks/dllmain.cpp`, `src/pnsradmatchmaking_hooks/logging.{h,cpp}`, `src/pnsradmatchmaking_hooks/hook_manager.{h,cpp}`
  - Pre-commit: `cmake --build build --target pnsradmatchmaking_hooks`

---

- [ ] 3. Offset Table + Hook Installation Framework

  **What to do**:
  - Create `offsets.h` — centralized address table for pnsradmatchmaking.dll (base 0x180000000):
    ```cpp
    namespace offsets {
      // Encode path
      constexpr uintptr_t MAIN_ENCODER = 0x180089870;        // FUN_180089870
      constexpr uintptr_t SENDER_DISPATCH = 0x1800860d0;     // FUN_1800860d0

      // Decode handlers (7 WebSocket message types)
      constexpr uintptr_t DECODE_LOBBY_DIR = 0x180081700;     // hash: 0xecda9827d712e4cc
      constexpr uintptr_t DECODE_SESSION_FAIL = 0x180082570;  // hash: 0x4ae8365ebc45f96c
      constexpr uintptr_t DECODE_SESSION_OK = 0x180082660;    // hash: 0x6d4de3650ee3110f
      constexpr uintptr_t DECODE_MM_STATUS = 0x180080ab0;     // hash: 0x8f28cf33dabfbecb
      constexpr uintptr_t DECODE_PING_REQ = 0x180080b60;      // hash: 0xfabf5f8719bfebf3
      constexpr uintptr_t DECODE_PSESSIONS_FAIL = 0x180080f50; // hash: 0x861c1fda4afe610b
      constexpr uintptr_t DECODE_PSESSIONS_OK = 0x180081010;  // hash: 0xa1b9cae1f8588969

      // Infrastructure
      constexpr uintptr_t MSG_QUEUE_INSERT = 0x180085940;     // message_queue_insert_sorted
      constexpr uintptr_t GET_GLOBAL_QUEUE = 0x180084e30;     // get_global_message_queue
    }
    ```
  - Create `hooks.h` / `hooks.cpp` — hook installation using offset table:
    - `install_all_hooks(HMODULE dll_base)` — computes actual addresses from base + RVA
    - Resolve: `actual_addr = (uintptr_t)dll_base + (OFFSET - 0x180000000)`
    - Install encode hook on `MAIN_ENCODER`
    - Install 7 decode hooks on each `DECODE_*` address
    - Each hook function is a trampoline: call original → translate → return
    - Initial implementation: ALL hooks are PASSTHROUGH (call original, return result unchanged)
  - Create `hook_types.h` — typedef for each hooked function's signature:
    - Encoder: `typedef int (*fn_main_encoder)(void* ctx, void* msg_struct, uint64_t msg_hash, ...)`
    - Decoders: `typedef int (*fn_decode_handler)(void* ctx, void* raw_data, size_t len, ...)`
    - (Exact signatures derived from existing decompilation — see References)

  **Must NOT do**:
  - Do NOT hardcode absolute addresses — always compute from DLL base + RVA offset
  - Do NOT install non-passthrough hooks yet (that's Phase 2)
  - Do NOT touch any address outside pnsradmatchmaking.dll's address space

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Address computation + MinHook integration requires precision
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (Wave 1)
  - **Blocks**: Tasks 4, 5, 6
  - **Blocked By**: Task 2

  **References**:

  **Pattern References**:
  - `src/pnsradmatchmaking/Types/NetworkStructs.h` — Contains all 7 hash constants in `namespace evr::ws_message_types`, plus struct definitions for `PeerEntry`, `WebSocketManager`, `MessageQueueRecord`
  - `src/pnsradmatchmaking/Matchmaking/message_queue.cpp` — Shows how `message_queue_insert_sorted` uses the hash for dispatch

  **API/Type References**:
  - `NetworkStructs.h:WebSocketManager` — 0x150-byte struct, important for context parameter in hooks
  - `NetworkStructs.h:MessageQueueRecord` — 0x18-byte struct: `{uint64_t symbol_hash, void* handler_fn, void* context}`

  **External References**:
  - MinHook `MH_CreateHook(target, detour, &original)`: https://github.com/TsudaKageworthy/minhook
  - Windows `GetModuleHandleA("pnsradmatchmaking.dll")` for DLL base address

  **WHY Each Reference Matters**:
  - `NetworkStructs.h` provides the HASH CONSTANTS we're hooking against — must match exactly
  - `message_queue.cpp` shows the dispatch mechanism — understanding this is critical for knowing WHERE hooks intercept
  - MinHook API docs define the exact function signatures for hook creation

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Passthrough hooks compile and install without crash
    Tool: Bash (cmake + test loader)
    Preconditions: Task 2 DllMain works
    Steps:
      1. cmake --build build --target pnsradmatchmaking_hooks
      2. Assert: compiles without errors
      3. Assert: offsets.h contains all 7 DECODE_* constants
      4. Assert: hooks.cpp has install_all_hooks function
      5. grep -c "MH_CreateHook" src/pnsradmatchmaking_hooks/hooks.cpp
      6. Assert: count >= 8 (1 encode + 7 decode)
    Expected Result: All 8 hook points defined with passthrough trampolines
    Failure Indicators: Missing hook points, wrong offset values
    Evidence: Source file content and build output captured

  Scenario: RVA computation is correct
    Tool: Bash (test program)
    Preconditions: offsets.h exists
    Steps:
      1. Write test that verifies: for each offset, (OFFSET - 0x180000000) produces valid RVA
      2. Assert: all RVAs are positive and < DLL image size
      3. Assert: no RVA exceeds reasonable bounds (e.g., < 0x400000)
    Expected Result: All offsets produce valid RVAs
    Evidence: Test output captured
  ```

  **Commit**: YES
  - Message: `feat(hooks): add offset table and passthrough hook installation framework`
  - Files: `src/pnsradmatchmaking_hooks/offsets.h`, `src/pnsradmatchmaking_hooks/hooks.{h,cpp}`, `src/pnsradmatchmaking_hooks/hook_types.h`
  - Pre-commit: `cmake --build build --target pnsradmatchmaking_hooks`

---

### Phase 2: Core Translation

- [ ] 4. Encode Hook — SNS Binary → Protobuf (Outbound Path)

  **What to do**:
  - Create `encode_hook.h` / `encode_hook.cpp`:
    - Hook function replaces `FUN_180089870` (main encoder)
    - On each call:
      1. Read the `msg_hash` parameter (identifies message type)
      2. Look up hash in `hash_proto_map` (from Task 0)
      3. If MAPPED: translate SNS struct fields → protobuf `Envelope` message → serialize to bytes → send via WebSocket
      4. If UNMAPPED: wrap raw bytes in `SNSUnknownMessage` (proto field 13) with `{type_hash: uint64, data: bytes}`
    - Translation functions (one per message type):
      - `translate_lobby_create_session(void* sns_struct) → rtapi::v1::Envelope` 
      - (repeat for each outbound message type)
    - Wire format: serialize `Envelope` to bytes, prepend 4-byte length prefix (standard protobuf framing)
  - Create `sns_struct_readers.h` — helper functions to read fields from SNS binary structs:
    - Read string: offset → length-prefixed string extraction
    - Read uint64/uint32/int32: direct memory reads with proper alignment
    - Read arrays: count + pointer-to-elements pattern

  **Must NOT do**:
  - Do NOT modify the original SNS struct in memory (read-only access)
  - Do NOT block the calling thread (serialization must be fast)
  - Do NOT allocate large buffers on stack (use heap for protobuf serialization)
  - Do NOT send to UDP — only intercept WebSocket-bound messages

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Complex struct→proto field mapping with binary memory reading
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 5, 6)
  - **Blocks**: Task 7
  - **Blocked By**: Task 3

  **References**:

  **Pattern References**:
  - `src/pnsradmatchmaking/Types/NetworkStructs.h` — All struct definitions for SNS message payloads
  - Binary decompilation of `FUN_180089870` — Shows how encoder reads struct fields (field offsets, types, sizes)

  **API/Type References**:
  - `~/src/nevr-common/proto/rtapi/v1/realtime_v1.proto` — Protobuf message definitions. Key outbound types:
    - `LobbySessionCreateRequest` (field 7) — maps to `SNSLobbyCreateSessionRequestv9`
    - `LobbyFindSessionRequest` (field 26) — maps to lobby find
    - `LobbyJoinSessionRequest` (field 27) — maps to lobby join
    - `LobbyPingResponse` (field 32) — maps to ping reply
    - `SNSUnknownMessage` (field 13) — passthrough wrapper for unmapped types
  - `src/pnsradmatchmaking_hooks/hash_proto_map.h` (from Task 0) — Hash → proto field number mapping

  **Documentation References**:
  - Protobuf C++ tutorial: https://protobuf.dev/getting-started/cpptutorial/ — `SerializeToString()`, `set_*()` methods

  **WHY Each Reference Matters**:
  - `NetworkStructs.h` provides the EXACT memory layout of SNS structs we're reading from
  - `realtime_v1.proto` defines the OUTPUT format we're translating TO
  - `hash_proto_map.h` is the dispatch table that routes hash → translation function
  - The binary decompilation shows ACTUAL field offsets (not assumed — these are ground truth)

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Encode hook compiles and links
    Tool: Bash (cmake)
    Preconditions: Task 3 framework complete
    Steps:
      1. cmake --build build --target pnsradmatchmaking_hooks
      2. Assert: exit code 0
      3. Assert: encode_hook.cpp compiled without warnings (-Wall)
    Expected Result: Encode hook code compiles cleanly
    Evidence: Build output captured

  Scenario: SNS struct reader extracts known field correctly
    Tool: Bash (unit test)
    Preconditions: Test with known-good SNS binary blob
    Steps:
      1. Create test with hardcoded SNS binary struct (from captured packet)
      2. Call sns_reader functions to extract fields
      3. Assert: extracted fields match expected values
    Expected Result: Binary struct parsing produces correct field values
    Evidence: Test output captured to .sisyphus/evidence/task-4-encode.txt
  ```

  **Commit**: YES (groups with 5, 6)
  - Message: `feat(hooks): implement SNS→protobuf encode translation`
  - Files: `src/pnsradmatchmaking_hooks/encode_hook.{h,cpp}`, `src/pnsradmatchmaking_hooks/sns_struct_readers.h`
  - Pre-commit: `cmake --build build --target pnsradmatchmaking_hooks`

---

- [ ] 5. Decode Hooks — Protobuf → SNS Binary (Inbound Path, 7 Handlers)

  **What to do**:
  - Create `decode_hooks.h` / `decode_hooks.cpp`:
    - 7 hook functions, one per decode handler address:
      1. `hook_decode_lobby_dir(ctx, data, len)` → intercepts `DECODE_LOBBY_DIR` (0x180081700)
      2. `hook_decode_session_fail(ctx, data, len)` → intercepts `DECODE_SESSION_FAIL` (0x180082570)
      3. `hook_decode_session_ok(ctx, data, len)` → intercepts `DECODE_SESSION_OK` (0x180082660)
      4. `hook_decode_mm_status(ctx, data, len)` → intercepts `DECODE_MM_STATUS` (0x180080ab0)
      5. `hook_decode_ping_req(ctx, data, len)` → intercepts `DECODE_PING_REQ` (0x180080b60)
      6. `hook_decode_psessions_fail(ctx, data, len)` → intercepts `DECODE_PSESSIONS_FAIL` (0x180080f50)
      7. `hook_decode_psessions_ok(ctx, data, len)` → intercepts `DECODE_PSESSIONS_OK` (0x180081010)
    - Each hook:
      1. Receive raw bytes from WebSocket
      2. Detect format: check if bytes are valid protobuf (try `Envelope::ParseFromString`)
      3. If PROTOBUF: deserialize `Envelope` → extract inner message → translate to SNS struct → call original handler with translated struct
      4. If NOT PROTOBUF (legacy SNS binary): call original handler unchanged (backward compatibility)
  - Create `sns_struct_writers.h` — helper functions to write fields into SNS binary structs:
    - Allocate struct memory with `new uint8_t[struct_size]`
    - Write string: length-prefix + data
    - Write numeric fields at correct offsets
    - Write arrays: count + pointer-to-elements
  - Map protobuf fields to SNS struct fields for each of the 7 message types:
    - `LobbySessionState` (proto) → SNS lobby directory response struct
    - `SessionFailure` (proto) → SNS session failure struct
    - `SessionSuccess` (proto) → SNS session success struct
    - `MatchmakerStatus` (proto) → SNS matchmaker status struct
    - `PingRequest` (proto) → SNS ping request struct
    - `PlayerSessionsFailure` (proto) → SNS find player sessions failure struct
    - `PlayerSessionsSuccess` (proto) → SNS find player sessions success struct

  **Must NOT do**:
  - Do NOT leak allocated SNS structs (pair every `new` with corresponding `delete`)
  - Do NOT modify global state in decode hooks
  - Do NOT assume protobuf format — check first, fall back to original handler for legacy binary
  - Do NOT allocate SNS structs on stack (they can be large — use heap)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 7 parallel decode paths with proto→binary struct translation
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 4, 6)
  - **Blocks**: Task 7
  - **Blocked By**: Task 3

  **References**:

  **Pattern References**:
  - `src/pnsradmatchmaking/Types/NetworkStructs.h` — SNS struct definitions for all 7 message types (field offsets, sizes)
  - Binary decompilation of each decode handler (0x180081700, etc.) — Shows how each handler reads incoming binary data (field access patterns)

  **API/Type References**:
  - `~/src/nevr-common/proto/rtapi/v1/realtime_v1.proto` — Protobuf message definitions. Key inbound types:
    - `LobbySessionState` (field 3) → Lobby directory response
    - `SessionFailure` (field 34) → Session failure
    - `SessionSuccess` (field 35) → Session success
    - `MatchmakerStatus` (field 28/29) → Matchmaker status
    - `PingRequest` (field 31) → Ping request
    - `PlayerSessions` (field 33) → Player sessions success/failure
  - `src/pnsradmatchmaking_hooks/offsets.h` — All 7 decode handler addresses

  **WHY Each Reference Matters**:
  - `NetworkStructs.h` defines the EXACT struct layout we must CREATE from protobuf data
  - Binary decompilation shows how the ORIGINAL handler accesses the struct (verifies our memory layout)
  - `realtime_v1.proto` defines the INPUT format we're translating FROM
  - `offsets.h` ensures we hook the CORRECT addresses

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Decode hooks compile for all 7 message types
    Tool: Bash (cmake)
    Preconditions: Task 3 framework complete
    Steps:
      1. cmake --build build --target pnsradmatchmaking_hooks
      2. Assert: exit code 0
      3. grep -c "hook_decode_" src/pnsradmatchmaking_hooks/decode_hooks.cpp
      4. Assert: count >= 7
    Expected Result: All 7 decode hooks implemented and compiled
    Evidence: Build output captured

  Scenario: Legacy binary format falls through to original handler
    Tool: Bash (unit test)
    Preconditions: Unit test with known SNS binary blob
    Steps:
      1. Pass non-protobuf binary data to hook function
      2. Assert: original handler is called with unchanged data
      3. Assert: no crash, no data corruption
    Expected Result: Backward compatibility preserved
    Evidence: Test output captured to .sisyphus/evidence/task-5-decode-fallback.txt
  ```

  **Commit**: YES (groups with 4, 6)
  - Message: `feat(hooks): implement protobuf→SNS decode hooks for 7 message types`
  - Files: `src/pnsradmatchmaking_hooks/decode_hooks.{h,cpp}`, `src/pnsradmatchmaking_hooks/sns_struct_writers.h`
  - Pre-commit: `cmake --build build --target pnsradmatchmaking_hooks`

---

- [ ] 6. Passthrough + Unknown Message Handler

  **What to do**:
  - Create `passthrough.h` / `passthrough.cpp`:
    - `SNSUnknownMessage` handler for unmapped message types:
      - On ENCODE: wrap raw SNS binary as `Envelope.unknown_message = {type: hash_u64, data: raw_bytes}`
      - On DECODE: extract `unknown_message.data` bytes and pass to original handler unchanged
    - Logging: log unknown message type hash at DEBUG level for future mapping
  - Add `is_protobuf_envelope(const uint8_t* data, size_t len)` helper:
    - Fast check: try `Envelope::ParsePartialFromString` on first N bytes
    - Returns true if valid protobuf, false if legacy binary format
    - Used by all decode hooks for format detection
  - Ensure ALL message paths have a handler:
    - Known mapped types → full translation (Tasks 4, 5)
    - Unknown types → passthrough via `SNSUnknownMessage` wrapper
    - Legacy binary → direct passthrough to original handler

  **Must NOT do**:
  - Do NOT drop unknown messages (MUST forward them)
  - Do NOT attempt to decode unknown message contents (opaque bytes only)
  - Do NOT log message CONTENT at INFO level (security: may contain tokens/credentials)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Small, focused task — wrapper logic only
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 4, 5)
  - **Blocks**: Task 7
  - **Blocked By**: Task 3

  **References**:

  **Pattern References**:
  - `~/src/nevr-common/proto/rtapi/v1/realtime_v1.proto` — `SNSUnknownMessage` definition (field 13 in Envelope oneof)

  **API/Type References**:
  - Protobuf `ParsePartialFromString()` — for fast format detection without full validation

  **WHY Each Reference Matters**:
  - `SNSUnknownMessage` (proto field 13) is the DEFINED passthrough mechanism in the proto schema
  - `ParsePartialFromString` is faster than `ParseFromString` for detection (doesn't check required fields)

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Unknown message type is wrapped and forwarded
    Tool: Bash (unit test)
    Preconditions: Passthrough module compiled
    Steps:
      1. Create test with fabricated unknown SNS binary + made-up hash
      2. Call encode passthrough → assert wraps in SNSUnknownMessage
      3. Call decode passthrough on wrapped message → assert extracts original bytes
      4. Assert: round-trip produces identical bytes
    Expected Result: Unknown messages survive encode→decode round-trip unchanged
    Evidence: Test output captured to .sisyphus/evidence/task-6-passthrough.txt
  ```

  **Commit**: YES (groups with 4, 5)
  - Message: `feat(hooks): add passthrough handler for unknown SNS message types`
  - Files: `src/pnsradmatchmaking_hooks/passthrough.{h,cpp}`
  - Pre-commit: `cmake --build build --target pnsradmatchmaking_hooks`

---

### Phase 3: Testing & Integration

- [ ] 7. Unit Test Suite (All Message Types)

  **What to do**:
  - Create `tests/test_protobuf_hooks.cpp`:
    - GoogleTest suite with tests for each message translation path
    - **Encode tests** (SNS→Proto):
      - For each outbound message type: create known SNS struct → call encode → deserialize protobuf → assert fields match
    - **Decode tests** (Proto→SNS):
      - For each of 7 inbound types: create protobuf `Envelope` → serialize → call decode hook → read resulting SNS struct → assert fields match
    - **Round-trip tests**:
      - SNS→Proto→SNS: original fields preserved
      - Proto→SNS→Proto: original fields preserved
    - **Passthrough tests**:
      - Unknown message encode/decode round-trip
      - Legacy binary format detection + passthrough
    - **Edge case tests**:
      - Empty protobuf message → should not crash
      - Truncated protobuf → should fall back to legacy handler
      - Maximum-size message → should handle without stack overflow
      - Missing required proto fields → should handle gracefully (log warning, pass through)
  - Add test target to CMakeLists.txt:
    - `add_executable(test_protobuf_hooks tests/test_protobuf_hooks.cpp)`
    - Link: `pnsradmatchmaking_hooks`, `GTest::gtest_main`, `protobuf::libprotobuf-lite`
    - `add_test(NAME protobuf_hooks COMMAND test_protobuf_hooks)`

  **Must NOT do**:
  - Do NOT test with live game process (that's Task 8)
  - Do NOT mock MinHook — test translation logic directly
  - Do NOT skip edge cases (truncated/empty/malformed inputs)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Comprehensive test suite with many test cases
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (Wave 3)
  - **Blocks**: Task 8
  - **Blocked By**: Tasks 4, 5, 6

  **References**:

  **Pattern References**:
  - `src/pnsradmatchmaking/Types/NetworkStructs.h` — Struct definitions for creating test data
  - `~/src/nevr-common/proto/rtapi/v1/realtime_v1.proto` — Proto definitions for creating test messages

  **External References**:
  - GoogleTest assertions: https://google.github.io/googletest/reference/assertions.html

  **WHY Each Reference Matters**:
  - Test data must match EXACT struct layouts from `NetworkStructs.h`
  - Proto definitions tell us what fields to set and verify

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Full test suite passes
    Tool: Bash (ctest)
    Preconditions: All Phase 2 tasks complete
    Steps:
      1. cmake --build build --target test_protobuf_hooks
      2. ctest --test-dir build -R protobuf_hooks --output-on-failure
      3. Assert: exit code 0
      4. Assert: output shows "X tests passed, 0 failures"
      5. Assert: X >= 20 (minimum test count: 7 encode + 7 decode + 7 round-trip + edge cases)
    Expected Result: All tests pass, zero failures
    Failure Indicators: Any test failure, crash, timeout
    Evidence: ctest output captured to .sisyphus/evidence/task-7-tests.txt

  Scenario: Edge cases don't crash
    Tool: Bash (ctest with filter)
    Preconditions: Test suite compiled
    Steps:
      1. ctest --test-dir build -R "edge_case" --output-on-failure
      2. Assert: no SEGFAULT, no ASAN errors
      3. Assert: graceful handling (log warning, pass through)
    Expected Result: All edge cases handled without crashes
    Evidence: Test output captured
  ```

  **Commit**: YES
  - Message: `test(hooks): add comprehensive unit tests for protobuf translation`
  - Files: `tests/test_protobuf_hooks.cpp`, `src/pnsradmatchmaking_hooks/CMakeLists.txt` (test target)
  - Pre-commit: `ctest --test-dir build -R protobuf_hooks`

---

- [ ] 8. Mock Proto Server + Integration Test

  **What to do**:
  - Create `tests/mock_proto_server.py` (Python, simple):
    - WebSocket server using `websockets` library
    - Listens on `ws://127.0.0.1:6789`
    - Receives protobuf `Envelope` messages from hooked client
    - Logs received message types and field summaries
    - Responds with hardcoded protobuf `Envelope` responses:
      - `LobbySessionState` (lobby directory)
      - `PingRequest` → `PingResponse`
      - `SessionSuccess` (for create/join session)
    - Validates: received messages are valid protobuf (not raw SNS binary)
  - Create `tests/test_integration.py` or `tests/test_integration.sh`:
    - Start mock server
    - Inject hook DLL into test process (or use test loader that loads pnsradmatchmaking.dll + hooks)
    - Send test messages through the WebSocket path
    - Verify:
      - Server receives protobuf `Envelope` (not raw binary)
      - Client receives translated SNS structs (via logging)
      - Round-trip latency < 5ms per message
  - Document injection method for live testing:
    - Windows: Use `LoadLibraryA` injection or detours-based loader
    - Test harness: Create standalone test that loads both DLLs in-process

  **Must NOT do**:
  - Do NOT require echovr.exe running for this test (use mock/test harness)
  - Do NOT connect to any real game server
  - Do NOT test with real player credentials

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Cross-language integration (C++ DLL + Python server)
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (Wave 3)
  - **Blocks**: Task 9
  - **Blocked By**: Task 7

  **References**:

  **Pattern References**:
  - `~/src/nevr-common/proto/rtapi/v1/realtime_v1.proto` — Proto definitions for server response messages

  **External References**:
  - Python `websockets` library: https://websockets.readthedocs.io/
  - Python protobuf: `pip install protobuf`, generate with `protoc --python_out=.`

  **WHY Each Reference Matters**:
  - `realtime_v1.proto` defines the messages the mock server must understand and respond with
  - `websockets` is the simplest Python WebSocket server for testing

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Mock server receives protobuf from hooked client
    Tool: Bash (python + test harness)
    Preconditions: Hook DLL built, Python protobuf generated
    Steps:
      1. Start mock server: python tests/mock_proto_server.py &
      2. Run test harness that loads hook DLL and sends test message
      3. Assert: server log shows "Received Envelope: type=LobbySessionCreateRequest"
      4. Assert: server log does NOT show "Invalid protobuf" or raw binary
      5. Kill mock server
    Expected Result: Server receives valid protobuf Envelope messages
    Failure Indicators: Server receives raw binary, parse errors, connection refused
    Evidence: Server log captured to .sisyphus/evidence/task-8-integration.txt

  Scenario: Client processes protobuf response correctly
    Tool: Bash (test harness + log check)
    Preconditions: Mock server running, hook DLL loaded
    Steps:
      1. Mock server sends PingRequest as protobuf Envelope
      2. Hook DLL receives, translates to SNS struct, calls original handler
      3. Assert: hook log shows "Decoded PingRequest → SNS struct"
      4. Assert: no crash, no data corruption
    Expected Result: Protobuf→SNS translation works end-to-end
    Evidence: Hook log captured
  ```

  **Commit**: YES
  - Message: `test(hooks): add mock protobuf server and integration tests`
  - Files: `tests/mock_proto_server.py`, `tests/test_integration.sh`
  - Pre-commit: `python tests/mock_proto_server.py --self-test`

---

- [ ] 9. End-to-End Smoke Test + Documentation

  **What to do**:
  - Create `docs/protobuf_hooks_guide.md`:
    - Architecture overview (what hooks where, data flow diagram)
    - Build instructions (`cmake -B build -S . && cmake --build build --target pnsradmatchmaking_hooks`)
    - Injection instructions (how to load DLL into echovr.exe)
    - Configuration (log level, log path)
    - Adding new message types (step-by-step for mapping a new SNS hash → proto field)
    - Troubleshooting (common issues, log analysis)
  - Create `src/pnsradmatchmaking_hooks/README.md`:
    - File listing with descriptions
    - Quick reference: which file handles which message type
  - Run end-to-end smoke test:
    - Build everything from clean
    - Run unit tests
    - Run integration test with mock server
    - Verify all tests pass
    - Verify DLL size is reasonable (< 5MB with protobuf)
  - Final cleanup:
    - Remove any TODO/FIXME comments that are resolved
    - Ensure all files have consistent code style
    - Verify no compiler warnings with `-Wall -Wextra`

  **Must NOT do**:
  - Do NOT test with live game servers (mock only)
  - Do NOT include credentials or server URLs in documentation
  - Do NOT create overly long documentation (keep it practical)

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: Documentation-heavy task
  - **Skills**: `[]`

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (Wave 3, final)
  - **Blocks**: None (final task)
  - **Blocked By**: Task 8

  **References**:

  **Pattern References**:
  - All `src/pnsradmatchmaking_hooks/` files from Tasks 0-8

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Clean build from scratch succeeds
    Tool: Bash (cmake)
    Preconditions: None (clean state)
    Steps:
      1. rm -rf build
      2. cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
      3. cmake --build build --target pnsradmatchmaking_hooks
      4. cmake --build build --target test_protobuf_hooks
      5. ctest --test-dir build -R protobuf_hooks --output-on-failure
      6. Assert: all steps exit code 0
    Expected Result: Full clean build + all tests pass
    Failure Indicators: Any build error, any test failure
    Evidence: Full output captured to .sisyphus/evidence/task-9-e2e.txt

  Scenario: Documentation covers all required sections
    Tool: Bash (grep)
    Preconditions: docs/protobuf_hooks_guide.md exists
    Steps:
      1. grep -c "## Architecture" docs/protobuf_hooks_guide.md → >= 1
      2. grep -c "## Build" docs/protobuf_hooks_guide.md → >= 1
      3. grep -c "## Injection" docs/protobuf_hooks_guide.md → >= 1
      4. grep -c "## Adding New Message" docs/protobuf_hooks_guide.md → >= 1
      5. grep -c "## Troubleshooting" docs/protobuf_hooks_guide.md → >= 1
    Expected Result: All 5 required sections present
    Evidence: grep output captured
  ```

  **Commit**: YES
  - Message: `docs(hooks): add protobuf hooks architecture guide and finalize`
  - Files: `docs/protobuf_hooks_guide.md`, `src/pnsradmatchmaking_hooks/README.md`
  - Pre-commit: `ctest --test-dir build -R protobuf_hooks`

---

## Commit Strategy

| After Task | Message | Files | Verification |
|------------|---------|-------|--------------|
| 0 | `feat(hooks): validate SNS hash algorithm against known message types` | hash_validator.cpp, hash_proto_map.h | `./build/hash_validator` |
| 1 | `feat(hooks): add CMake target for protobuf WebSocket hook DLL` | CMakeLists.txt, dllmain.cpp | `cmake --build build --target pnsradmatchmaking_hooks` |
| 2 | `feat(hooks): implement DllMain lifecycle with MinHook and file logging` | dllmain.cpp, logging.*, hook_manager.* | `cmake --build build --target pnsradmatchmaking_hooks` |
| 3 | `feat(hooks): add offset table and passthrough hook installation framework` | offsets.h, hooks.*, hook_types.h | `cmake --build build --target pnsradmatchmaking_hooks` |
| 4+5+6 | `feat(hooks): implement SNS↔protobuf translation layer (encode + 7 decode + passthrough)` | encode_hook.*, decode_hooks.*, passthrough.*, sns_struct_*.h | `cmake --build build --target pnsradmatchmaking_hooks` |
| 7 | `test(hooks): add comprehensive unit tests for protobuf translation` | test_protobuf_hooks.cpp | `ctest -R protobuf_hooks` |
| 8 | `test(hooks): add mock protobuf server and integration tests` | mock_proto_server.py, test_integration.sh | `python mock_proto_server.py --self-test` |
| 9 | `docs(hooks): add protobuf hooks architecture guide and finalize` | protobuf_hooks_guide.md, README.md | `ctest -R protobuf_hooks` |

---

## Success Criteria

### Verification Commands
```bash
# Build hook DLL
cmake --build build --target pnsradmatchmaking_hooks  # Expected: BUILD SUCCESSFUL

# Run hash validation
./build/hash_validator  # Expected: 7/7 hashes matched

# Run unit tests
ctest --test-dir build -R protobuf_hooks --output-on-failure  # Expected: All tests pass

# Run integration test
python tests/mock_proto_server.py --self-test  # Expected: Self-test passed

# Verify DLL exports
dumpbin /EXPORTS build/src/pnsradmatchmaking_hooks/pnsradmatchmaking_hooks.dll  # Expected: DllMain exported

# Check DLL size
ls -la build/src/pnsradmatchmaking_hooks/pnsradmatchmaking_hooks.dll  # Expected: < 5MB
```

### Final Checklist
- [ ] All "Must Have" items present (hash validation, thread safety, logging, passthrough, clean unhook, unit tests)
- [ ] All "Must NOT Have" items absent (no UDP touching, no custom allocators, no disk patching, no blocking ops)
- [ ] Hash algorithm validated against all 7 known message types
- [ ] All 7 decode hooks implemented with backward compatibility (legacy format detection)
- [ ] Encode hook handles both mapped and unmapped message types
- [ ] Unit tests cover all 8 hook paths + edge cases
- [ ] Integration test with mock proto server passes
- [ ] Documentation complete with build/inject/configure/extend instructions
- [ ] Clean build from scratch succeeds with zero warnings

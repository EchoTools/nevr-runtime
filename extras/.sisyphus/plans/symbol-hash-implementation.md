# Symbol Hash Implementation

## TL;DR

> **Quick Summary**: Port CSymbol64_Hash into nevr-runtime as a self-contained C++17 constexpr header, then replace all hardcoded hex symbol constants with `symbol_hash("string")` calls for the 8 known pairs. Rename Tcp::GameClientMsg1/2/3 to their real protocol names. Mark 23 unresolved hashes with TODO comments.
> 
> **Deliverables**:
> - `src/common/symbol_hash.h` — constexpr CSymbol64_Hash implementation with static_assert validation
> - Updated `src/common/symbols.h` — 8 hashes replaced with symbol_hash(), 3 Tcp constants renamed, 23 unknowns annotated
> - Updated `src/gameserver/constants.h` — SYMBOL_SERVERDB replaced with symbol_hash()
> - Updated `src/gameserver/gameserver.cpp` — SYM_PROTOBUF_MSG replaced with symbols.h reference
> 
> **Estimated Effort**: Short
> **Parallel Execution**: NO — sequential (each task builds on the previous)
> **Critical Path**: Task 1 → Task 2 → Task 3 → Task 4

---

## Context

### Original Request
Port the CSymbol64_Hash algorithm into nevr-runtime and replace hardcoded hex constants with compile-time hash calls.

### Interview Summary
**Key Discussions**:
- API style: `symbol_hash("string")` function call (most idiomatic C++17 constexpr)
- Unresolved hashes: Keep current names, add `// TODO: unresolved hash` comments
- Tcp::GameClientMsg1/2/3: Rename to real protocol names (SNSLobbySettingsResponse, etc.)
- SYM_PROTOBUF_MSG: Replace with reference to `EchoVR::Symbols::NEVRProtobufMessageV1`
- Verification: static_assert only (compile-time, no runtime tests)
- Hook (Phase A) deferred — only doing implementation and constant replacement now

**Research Findings**:
- Authoritative C++ reference: `~/src/evr-reconstruction/tools/hash_tool/csymbol64_hash.cpp` — complete implementation with constexpr-compatible table generation and hash function
- Authoritative Go reference: `~/src/nakama/server/evr/core_hash.go` + `core_hash_lookup.go`
- `symbols.h` is currently dead code (included by `messages.h` but never referenced via `Sym::` or `TcpSym::`)
- `SYMBOL_SERVERDB` in constants.h is defined but never referenced in non-legacy code
- Both are safe to modify with zero runtime risk

### Metis Review
**Identified Gaps** (addressed):
- Type mismatch: `symbol_hash()` returns `uint64_t`, `SymbolId` is `INT64` — resolved: matches existing pattern where hex literals are already assigned to SymbolId
- Tcp negative decimal literals: `-5369924845641990433` in Tcp:: — resolved: these are "unknown" hashes, get TODO comment, value unchanged
- Dead code risk: symbols.h is unused — resolved: static_asserts provide compile-time validation regardless
- GameClientMsg naming: Confirmed rename to real protocol names

---

## Work Objectives

### Core Objective
Create a self-contained constexpr CSymbol64_Hash implementation and use it to replace hardcoded hex constants with readable `symbol_hash("protocol_name")` calls.

### Concrete Deliverables
- NEW: `src/common/symbol_hash.h`
- MODIFIED: `src/common/symbols.h`
- MODIFIED: `src/gameserver/constants.h`
- MODIFIED: `src/gameserver/gameserver.cpp`

### Definition of Done
- [ ] `make build` succeeds with zero errors and zero narrowing/signed warnings
- [ ] All 8 known hash pairs validated via static_assert at compile time
- [ ] No raw hex constants remain for the 8 known hashes in modified files
- [ ] Zero changes to `src/legacy/` directory
- [ ] Tcp::GameClientMsg1/2/3 renamed to real protocol names

### Must Have
- constexpr lookup table generation from polynomial `0x95ac9329ac4bc9b5`
- constexpr hash function: case-insensitive, default seed `0xFFFFFFFFFFFFFFFF`
- static_assert validation for all 8 known pairs
- `#pragma once` / include guard
- Self-contained header (only `<cstdint>` and `<array>` dependencies)

### Must NOT Have (Guardrails)
- DO NOT modify any file under `src/legacy/`
- DO NOT change the `SymbolId` typedef in `echovr.h`
- DO NOT add hash pairs beyond the 8 explicitly specified
- DO NOT add runtime (non-constexpr) functionality
- DO NOT add UDL operators (`"string"_sym`)
- DO NOT add dependencies on echovr.h from symbol_hash.h
- DO NOT modify gamepatches (frozen module)

---

## Verification Strategy (MANDATORY)

> **UNIVERSAL RULE: ZERO HUMAN INTERVENTION**
>
> ALL verification is compile-time via static_assert and build success. No runtime tests.

### Test Decision
- **Infrastructure exists**: N/A (compile-time only)
- **Automated tests**: None (static_assert is the verification)
- **Framework**: None needed

### Agent-Executed QA Scenarios (MANDATORY — ALL tasks)

**Verification Tool by Deliverable Type:**

| Type | Tool | How Agent Verifies |
|------|------|-------------------|
| Header file | Bash (make build) | Full project build succeeds |
| Constant replacement | Bash (grep) | Verify hex constants removed from target files |
| Legacy untouched | Bash (git diff) | Verify zero changes in src/legacy/ |

---

## Execution Strategy

### Sequential Execution

```
Task 1: Create symbol_hash.h
  ↓
Task 2: Update symbols.h (depends on Task 1)
  ↓
Task 3: Update constants.h (depends on Task 1)
  ↓
Task 4: Update gameserver.cpp (depends on Task 2)
```

All tasks are sequential — each depends on the previous.

### Dependency Matrix

| Task | Depends On | Blocks | Can Parallelize With |
|------|------------|--------|---------------------|
| 1 | None | 2, 3, 4 | None |
| 2 | 1 | 4 | 3 (after Task 1) |
| 3 | 1 | None | 2 (after Task 1) |
| 4 | 2 | None | None |

### Agent Dispatch Summary

| Wave | Tasks | Recommended Agents |
|------|-------|-------------------|
| 1 | 1, 2, 3, 4 | Single agent, sequential: `task(category="quick", load_skills=[], ...)` |

---

## TODOs

- [ ] 1. Create `src/common/symbol_hash.h`

  **What to do**:
  - Create a new self-contained header file `src/common/symbol_hash.h`
  - Include only `<cstdint>` and `<array>` — NO dependency on echovr.h
  - Implement `constexpr std::array<uint64_t, 256> generate_csymbol64_table()`:
    - CRC64 polynomial: `0x95ac9329ac4bc9b5ULL`
    - Port directly from the reference implementation's table generation algorithm
  - Implement `constexpr uint64_t symbol_hash(const char* str, uint64_t seed = 0xFFFFFFFFFFFFFFFFULL)`:
    - Case-insensitive: convert A-Z to a-z before hashing
    - Use the same lowercase check as the reference: `(uint8_t)(c + 0xbf) <= 0x19` (matches decompiled binary)
    - Hash update: `seed = (uint64_t)c ^ table[(seed >> 56) & 0xFF] ^ (seed << 8)`
    - Return empty string as seed (matches reference behavior)
  - Add 8 static_assert validations at the bottom of the file:
    ```cpp
    static_assert(symbol_hash("serverdb") == 0x25e886012ced8064ULL);
    static_assert(symbol_hash("SNSLobbyRegistrationSuccess") == 0xb57a31cdd0f6fedfULL);
    static_assert(symbol_hash("SNSLobbySessionSuccessv5") == 0x6d4de3650ee3110fULL);
    static_assert(symbol_hash("SNSLobbySettingsResponse") == 0x5b71b22a4483bda5ULL);
    static_assert(symbol_hash("SNSServerSettingsResponsev2") == 0x0a88cb5d166cc2caULL);
    static_assert(symbol_hash("SNSRewardsSettings") == 0xa7a9e5a70b2429dbULL);
    static_assert(symbol_hash("NEVRProtobufJSONMessageV1") == 0xc6b3710cd9c4ef47ULL);
    static_assert(symbol_hash("NEVRProtobufMessageV1") == 0x9ee5107d9e29fd63ULL);
    ```
  - Wrap everything in `namespace EchoVR { ... }` to match project conventions

  **Must NOT do**:
  - Do NOT include echovr.h
  - Do NOT add runtime-only functions
  - Do NOT add UDL operators
  - Do NOT use C++20 features

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Single new file, well-defined algorithm with reference implementation to port
  - **Skills**: none needed
  - **Skills Evaluated but Omitted**:
    - `frontend-ui-ux`: No UI work
    - `playwright`: No browser work

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (first task)
  - **Blocks**: Tasks 2, 3, 4
  - **Blocked By**: None

  **References** (CRITICAL):

  **Pattern References** (existing code to follow):
  - `~/src/evr-reconstruction/tools/hash_tool/csymbol64_hash.cpp:12-58` — Table generation algorithm. Port this EXACTLY as constexpr. The bit-level logic (0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 masks with XOR against polynomial) produces the correct 256-entry lookup table.
  - `~/src/evr-reconstruction/tools/hash_tool/csymbol64_hash.cpp:70-89` — Hash function algorithm. Port this as constexpr. Note the lowercase check `(uint8_t)(c + 0xbf) <= 0x19` which matches the decompiled binary.
  - `~/src/nakama/server/evr/core_hash.go:1-54` — Go reference implementation for cross-validation. The `CalculateSymbolValue` function and `generateHashPreCache` function confirm the algorithm.
  - `~/src/nakama/server/evr/core_hash_lookup.go:1-30` — Go lookup table generator for cross-validation.

  **API/Type References**:
  - `src/common/echovr.h:146` — `typedef INT64 SymbolId;` — The consuming code assigns `uint64_t` results to `SymbolId` (INT64). This is the existing pattern (hex literals assigned to signed type). `symbol_hash()` returns `uint64_t`; the implicit narrowing at assignment site matches existing code.

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios (MANDATORY):**

  ```
  Scenario: symbol_hash.h compiles and static_asserts pass
    Tool: Bash
    Preconditions: MinGW toolchain available, CMake configured
    Steps:
      1. make build
      2. Assert: Build succeeds with exit code 0
      3. Assert: No warnings containing "narrowing" or "signed" in build output
    Expected Result: Clean build, all 8 static_asserts pass (compile-time)
    Evidence: Build log captured

  Scenario: Header is self-contained
    Tool: Bash (grep)
    Preconditions: File exists at src/common/symbol_hash.h
    Steps:
      1. grep -c '#include' src/common/symbol_hash.h
      2. Assert: Only includes are <cstdint> and <array> (count = 2)
      3. grep 'echovr.h' src/common/symbol_hash.h
      4. Assert: Zero matches
    Expected Result: No dependency on echovr.h or other project headers
    Evidence: grep output captured
  ```

  **Commit**: YES
  - Message: `feat(common): add constexpr CSymbol64_Hash implementation`
  - Files: `src/common/symbol_hash.h`
  - Pre-commit: `make build`

---

- [ ] 2. Update `src/common/symbols.h`

  **What to do**:
  - Add `#include "symbol_hash.h"` at the top (after existing includes)
  - Replace the 8 known hashes with `symbol_hash()` calls:
    - `NEVRProtobufJSONMessageV1`: `0xc6b3710cd9c4ef47` → `symbol_hash("NEVRProtobufJSONMessageV1")`
  - In `namespace Tcp`:
    - `LobbyRegistrationSuccess`: `-5369924845641990433` → `symbol_hash("SNSLobbyRegistrationSuccess")` — verified: `(int64_t)0xb57a31cdd0f6fedf == -5369924845641990433`
    - `LobbySessionSuccessV5`: `0x6d4de3650ee3110f` → `symbol_hash("SNSLobbySessionSuccessv5")`
    - Rename `GameClientMsg1` → `SNSLobbySettingsResponse`: `0x5b71b22a4483bda5` → `symbol_hash("SNSLobbySettingsResponse")`
    - Rename `GameClientMsg2` → `SNSServerSettingsResponsev2`: `0xa88cb5d166cc2ca` → `symbol_hash("SNSServerSettingsResponsev2")`
    - Rename `GameClientMsg3` → `SNSRewardsSettings`: `0xa7a9e5a70b2429db` → `symbol_hash("SNSRewardsSettings")`
  - Note: `Tcp::LobbyRegistrationFailure` uses negative decimal literal (`-5373034290044534839` = `0xb56f25c7dfe6ffc9`). This is a DIFFERENT hash from main namespace `LobbyRegistrationFailure` (`0xcc3a40870cdbc852`). It is unresolved — add `// TODO: unresolved hash` comment.
  - For all 23 remaining hashes in the main `EchoVR::Symbols` namespace with unknown input strings, add `// TODO: unresolved hash` comment after each. Keep existing names and hex values unchanged. The 23 unknowns are ALL constants in the main namespace (lines 12-54 except NEVRProtobufJSONMessageV1 on line 57).
  - Update the comment for `GameClientMsg1/2/3` section header from "Game client messages" to "SNS settings messages" or similar

  **Must NOT do**:
  - Do NOT change hex values for unresolved hashes
  - Do NOT rename unresolved hash constants (only add TODO comment)
  - Do NOT change the negative decimal literals to hex
  - Do NOT modify the namespace structure

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Straightforward find-and-replace in a single file
  - **Skills**: none needed

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (after Task 1)
  - **Blocks**: Task 4
  - **Blocked By**: Task 1

  **References** (CRITICAL):

  **Pattern References**:
  - `src/common/symbols.h:1-76` — FULL FILE. This is the target file. Read the entire file before making changes. Note the namespace structure: `EchoVR::Symbols` (main) and `EchoVR::Symbols::Tcp` (sub-namespace).

  **Known hash-string pairs for replacement** (from brute-force research):
  - Line 57: `NEVRProtobufJSONMessageV1 = 0xc6b3710cd9c4ef47` → `symbol_hash("NEVRProtobufJSONMessageV1")`
  - Line 64: `Tcp::LobbySessionSuccessV5 = 0x6d4de3650ee3110f` → `symbol_hash("SNSLobbySessionSuccessv5")`
  - Line 67: `Tcp::GameClientMsg1 = 0x5b71b22a4483bda5` → rename to `SNSLobbySettingsResponse`, value `symbol_hash("SNSLobbySettingsResponse")`
  - Line 68: `Tcp::GameClientMsg2 = 0xa88cb5d166cc2ca` → rename to `SNSServerSettingsResponsev2`, value `symbol_hash("SNSServerSettingsResponsev2")`
  - Line 69: `Tcp::GameClientMsg3 = 0xa7a9e5a70b2429db` → rename to `SNSRewardsSettings`, value `symbol_hash("SNSRewardsSettings")`

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios (MANDATORY):**

  ```
  Scenario: symbols.h compiles with symbol_hash() calls
    Tool: Bash
    Preconditions: Task 1 complete (symbol_hash.h exists)
    Steps:
      1. make build
      2. Assert: Build succeeds with exit code 0
    Expected Result: Clean build
    Evidence: Build log captured

  Scenario: Known hashes replaced, unknowns annotated
    Tool: Bash (grep)
    Steps:
      1. grep -c 'symbol_hash(' src/common/symbols.h
      2. Assert: Count is 7 (NEVRProtobufJSONMessageV1 + NEVRProtobufMessageV1 + Tcp::LobbyRegistrationSuccess + Tcp::LobbySessionSuccessV5 + 3 renamed Tcp constants)
      3. grep -c 'TODO: unresolved hash' src/common/symbols.h
      4. Assert: Count is >= 23 (all unknown hashes annotated)
      5. grep 'GameClientMsg' src/common/symbols.h
      6. Assert: Zero matches (all renamed)
    Expected Result: All known hashes use symbol_hash(), all unknowns have TODO
    Evidence: grep output captured

  Scenario: Legacy files untouched
    Tool: Bash (git diff)
    Steps:
      1. git diff src/legacy/
      2. Assert: Empty output (no changes)
    Expected Result: Zero changes to legacy directory
    Evidence: git diff output captured
  ```

  **Commit**: YES
  - Message: `refactor(common): replace known symbol hashes with symbol_hash() calls`
  - Files: `src/common/symbols.h`
  - Pre-commit: `make build`

---

- [ ] 3. Update `src/gameserver/constants.h`

  **What to do**:
  - Add `#include "common/symbol_hash.h"` (or appropriate relative path based on include directory setup — check CMakeLists.txt include paths: `"${CMAKE_SOURCE_DIR}/src"` is in the include path, so use `"common/symbol_hash.h"`)
  - Replace `constexpr int64_t SYMBOL_SERVERDB = 0x25E886012CED8064;` with `constexpr int64_t SYMBOL_SERVERDB = EchoVR::symbol_hash("serverdb");`
  - Note: Keep the type as `int64_t` to match existing API. The implicit conversion from `uint64_t` to `int64_t` matches the existing pattern.

  **Must NOT do**:
  - Do NOT change other constants in the file
  - Do NOT change the type of SYMBOL_SERVERDB
  - Do NOT restructure the file

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Single line replacement in a small file
  - **Skills**: none needed

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 2, after Task 1)
  - **Parallel Group**: Wave 2 (with Task 2)
  - **Blocks**: None
  - **Blocked By**: Task 1

  **References** (CRITICAL):

  **Pattern References**:
  - `src/gameserver/constants.h:1-28` — FULL FILE. Target file. Line 23: `constexpr int64_t SYMBOL_SERVERDB = 0x25E886012CED8064;`
  - `CMakeLists.txt:173-175` — Include directories: `"${CMAKE_SOURCE_DIR}/src"` is in the include path, so `#include "common/symbol_hash.h"` will resolve correctly.

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios (MANDATORY):**

  ```
  Scenario: constants.h compiles with symbol_hash()
    Tool: Bash
    Preconditions: Task 1 complete
    Steps:
      1. make build
      2. Assert: Build succeeds
      3. grep '0x25E886012CED8064\|0x25e886012ced8064' src/gameserver/constants.h
      4. Assert: Zero matches (hex constant replaced)
      5. grep 'symbol_hash("serverdb")' src/gameserver/constants.h
      6. Assert: One match
    Expected Result: Hex constant replaced with symbol_hash call
    Evidence: grep output captured
  ```

  **Commit**: YES (group with Task 2)
  - Message: `refactor(gameserver): replace SYMBOL_SERVERDB with symbol_hash()`
  - Files: `src/gameserver/constants.h`
  - Pre-commit: `make build`

---

- [ ] 4. Update `src/gameserver/gameserver.cpp`

  **What to do**:
  - Add `#include "common/symbols.h"` if not already included (check existing includes first)
  - Replace line 51: `constexpr EchoVR::SymbolId SYM_PROTOBUF_MSG = 0x9ee5107d9e29fd63ULL;`
    with: `constexpr EchoVR::SymbolId SYM_PROTOBUF_MSG = EchoVR::Symbols::NEVRProtobufMessageV1;`
  - Alternative if `symbols.h` is already transitively included via `messages.h`: just change the value, no new include needed. Check the include chain first.

  **Must NOT do**:
  - Do NOT rename `SYM_PROTOBUF_MSG` — it's used throughout gameserver.cpp
  - Do NOT change any other code in gameserver.cpp
  - Do NOT remove the variable (keep it as a local alias)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Single line change
  - **Skills**: none needed

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (after Task 2)
  - **Blocks**: None
  - **Blocked By**: Task 2 (needs symbols.h updated first so NEVRProtobufMessageV1 uses symbol_hash)

  **References** (CRITICAL):

  **Pattern References**:
  - `src/gameserver/gameserver.cpp:49-55` — Target area. Line 51: `constexpr EchoVR::SymbolId SYM_PROTOBUF_MSG = 0x9ee5107d9e29fd63ULL;`
  - `src/common/symbols.h:57` — `constexpr SymbolId NEVRProtobufJSONMessageV1 = 0xc6b3710cd9c4ef47;` — After Task 2, this becomes `symbol_hash("NEVRProtobufJSONMessageV1")`. The neighboring constant for binary protobuf (`NEVRProtobufMessageV1`) should also exist after Task 2 adds it.
  - `src/gameserver/messages.h` — Check if this file already includes symbols.h (it does: Metis confirmed `messages.h` includes `symbols.h`). If gameserver.cpp includes `messages.h`, then symbols.h is transitively available.

  **IMPORTANT NOTE**: `NEVRProtobufMessageV1` with hash `0x9ee5107d9e29fd63` is NOT currently in symbols.h — it only exists as `SYM_PROTOBUF_MSG` in gameserver.cpp. Task 2 must ADD a new entry `constexpr SymbolId NEVRProtobufMessageV1 = symbol_hash("NEVRProtobufMessageV1");` to symbols.h (in the "Protobuf transport" section, next to `ProtobufMsg` and `ProtobufJson`). Then Task 4 can reference it.

  Similarly, `Tcp::LobbyRegistrationSuccess` in symbols.h (line 62, value `-5369924845641990433`) has the SAME hash as `SNSLobbyRegistrationSuccess` (`0xb57a31cdd0f6fedf`) — they're the same value in different representations. The planning session identified `SNSLobbyRegistrationSuccess → 0xb57a31cdd0f6fedf` as a known pair. This means Tcp::LobbyRegistrationSuccess CAN be replaced with `symbol_hash("SNSLobbyRegistrationSuccess")`. Verify: `(int64_t)0xb57a31cdd0f6fedf == -5369924845641990433`. Task 2 should handle this.

  Similarly check `Tcp::LobbyRegistrationFailure = -5373034290044534839` — verify if this corresponds to any known hash. If `(uint64_t)(-5373034290044534839) == 0xcc3a40870cdbc852` (which is `LobbyRegistrationFailure` in the main namespace), then this is also a known... wait, both are UNKNOWN. Neither has a known input string. Leave both with TODO comments.

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios (MANDATORY):**

  ```
  Scenario: gameserver.cpp uses symbols.h reference
    Tool: Bash
    Preconditions: Tasks 1-2 complete
    Steps:
      1. make build
      2. Assert: Build succeeds
      3. grep '0x9ee5107d9e29fd63' src/gameserver/gameserver.cpp
      4. Assert: Zero matches (hex constant replaced)
      5. grep 'EchoVR::Symbols::NEVRProtobufMessageV1\|Symbols::NEVRProtobufMessageV1' src/gameserver/gameserver.cpp
      6. Assert: At least one match
    Expected Result: SYM_PROTOBUF_MSG now references symbols.h constant
    Evidence: grep output captured
  ```

  **Commit**: YES (group with Tasks 2+3)
  - Message: `refactor(gameserver): use symbols.h for protobuf message symbol`
  - Files: `src/gameserver/gameserver.cpp`
  - Pre-commit: `make build`

---

## Commit Strategy

| After Task | Message | Files | Verification |
|------------|---------|-------|--------------|
| 1 | `feat(common): add constexpr CSymbol64_Hash implementation` | `src/common/symbol_hash.h` | `make build` |
| 2, 3, 4 | `refactor: replace hardcoded symbol hashes with symbol_hash() calls` | `src/common/symbols.h`, `src/gameserver/constants.h`, `src/gameserver/gameserver.cpp` | `make build` |

---

## Success Criteria

### Verification Commands
```bash
make build                    # Expected: exit 0, no errors
grep -rn 'symbol_hash(' src/common/symbols.h src/gameserver/constants.h
                              # Expected: 8+ matches (7 in symbols.h, 1 in constants.h)
grep -rn '0x9ee5107d9e29fd63' src/gameserver/gameserver.cpp
                              # Expected: 0 matches
grep -rn '0x25E886012CED8064' src/gameserver/constants.h
                              # Expected: 0 matches
grep -rn 'GameClientMsg' src/common/symbols.h
                              # Expected: 0 matches (all renamed)
git diff src/legacy/          # Expected: empty (no changes)
```

### Final Checklist
- [ ] `symbol_hash.h` exists and is self-contained (only <cstdint> and <array>)
- [ ] All 8 static_asserts present and passing (compile-time verified)
- [ ] 7 symbol_hash() calls in symbols.h (NEVRProtobufJSONMessageV1, NEVRProtobufMessageV1, Tcp::LobbyRegistrationSuccess, Tcp::LobbySessionSuccessV5, and 3 renamed Tcp constants)
- [ ] 1 symbol_hash() call in constants.h (SYMBOL_SERVERDB)
- [ ] 1 NEVRProtobufMessageV1 entry added to symbols.h with symbol_hash()
- [ ] SYM_PROTOBUF_MSG references EchoVR::Symbols::NEVRProtobufMessageV1
- [ ] GameClientMsg1/2/3 renamed to SNSLobbySettingsResponse/SNSServerSettingsResponsev2/SNSRewardsSettings
- [ ] 23+ unresolved hashes annotated with `// TODO: unresolved hash`
- [ ] Zero changes to src/legacy/
- [ ] Zero changes to echovr.h

# Test Harness Integration for nevr-runtime

## TL;DR

> **Quick Summary**: Set up nevr-runtime to use evr-test-harness (via symlink) for automated system tests covering DLL loading, game patches, multiplayer, telemetry, and full E2E scenarios.
> 
> **Deliverables**:
> - Symlink `./extern/evr-test-harness` → `~/src/evr-test-harness`
> - Go test module at `tests/system/` with per-area test files
> - Makefile targets: `test-system`, `test-system-short`
> - Documentation in `tests/system/README.md`
> 
> **Estimated Effort**: Medium
> **Parallel Execution**: YES - 2 waves
> **Critical Path**: Task 1 → Task 2 → Task 3 → Tasks 4-8 (parallel) → Task 9

---

## Context

### Original Request
User wants nevr-runtime to use `~/src/evr-test-harness` for all automated system tests.

### Interview Summary
**Key Discussions**:
- Integration model: Symlink approach chosen (not git submodule)
- Test scope: All areas - DLL loading, patches, multiplayer, telemetry, E2E
- CI/CD: Local development only, no GitHub Actions integration
- Test location: `tests/system/` directory
- Test runner: Go tests using evr-test-harness's utilities
- Go module strategy: Sub-module with `replace` directive
- MCPClient strategy: User chose to refactor evr-test-harness first

**Research Findings**:
- nevr-runtime is a C++ DLL project with only manual shell test scripts
- evr-test-harness is a Go MCP server with 21 tools and 44 integration tests
- MCPClient/TestFixture code currently lives in `tests/integration/main_test.go` (not importable)
- Port ranges: 6721-6790 for game sessions, 7350-7351 for Nakama

### Metis Review
**Identified Gaps** (addressed):
- MCPClient code not importable: User chose "refactor harness first" → this plan assumes refactoring is done
- Symlink vs replace directive: Plan uses both (symlink for IDE, replace for Go)
- Existing test scripts overlap: New tests complement, don't replace existing shell scripts

---

## Prerequisites

> **⚠️ PREREQUISITE**: This plan assumes evr-test-harness has been refactored to export `pkg/testutil/` package.
> 
> Before executing this plan, complete the evr-test-harness refactoring work:
> 1. Run a separate planning session in `~/src/evr-test-harness`
> 2. Create `pkg/testutil/` with exported TestFixture, MCPClient
> 3. Update existing integration tests to use the new package
> 4. Verify all tests pass with `make test-integration`
>
> **Verification**: `ls ~/src/evr-test-harness/pkg/testutil/*.go` should show client.go, fixture.go

---

## Work Objectives

### Core Objective
Enable automated system testing of nevr-runtime DLLs using evr-test-harness's MCP-based game control capabilities.

### Concrete Deliverables
- `extern/evr-test-harness` symlink → `~/src/evr-test-harness`
- `tests/system/go.mod` with evr-test-harness dependency
- `tests/system/helpers_test.go` - shared test utilities
- `tests/system/dll_test.go` - DLL loading/injection tests
- `tests/system/patches_test.go` - game patch behavior tests
- `tests/system/multiplayer_test.go` - multiplayer session tests
- `tests/system/telemetry_test.go` - telemetry agent tests
- `tests/system/e2e_test.go` - full integration cycle tests
- `tests/system/README.md` - test workflow documentation
- Makefile targets: `test-system`, `test-system-short`

### Definition of Done
- [x] `cd tests/system && go test -v -short ./...` runs without errors
- [x] `make test-system-short` executes successfully
- [x] At least one test per area (5 total) is implemented and passes
- [x] README documents test workflow and prerequisites

### Must Have
- Symlink working and resolvable
- Go module compiles with evr-test-harness import
- Per-area test file structure
- Makefile integration
- Short mode support (skip long tests during quick iteration)

### Must NOT Have (Guardrails)
- NO CI/CD configuration changes (local development only)
- NO modifications to evr-test-harness (separate repo)
- NO removal of existing test-*.sh scripts (complementary, not replacement)
- NO hardcoded absolute paths in Go code (use environment variables)
- NO test implementations that require human intervention to verify

---

## Verification Strategy

### Test Decision
- **Infrastructure exists**: NO (new Go module created by this plan)
- **Automated tests**: YES (this IS the test infrastructure)
- **Framework**: Go testing package with testify assertions

### Agent-Executed QA Scenarios (MANDATORY)

All verification is automated. The executing agent will directly verify each deliverable.

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Start Immediately - Sequential Foundation):
└── Task 1: Create extern directory and symlink
    └── Task 2: Initialize Go module
        └── Task 3: Create helpers and verify imports

Wave 2 (After Wave 1 - Parallel Test Development):
├── Task 4: DLL loading tests
├── Task 5: Patch behavior tests
├── Task 6: Multiplayer tests
├── Task 7: Telemetry tests
└── Task 8: E2E tests

Wave 3 (After Wave 2 - Integration):
└── Task 9: Makefile targets and documentation

Critical Path: 1 → 2 → 3 → [4-8] → 9
Parallel Speedup: Tasks 4-8 can run simultaneously (~50% faster)
```

### Dependency Matrix

| Task | Depends On | Blocks | Can Parallelize With |
|------|------------|--------|---------------------|
| 1 | None | 2 | None |
| 2 | 1 | 3 | None |
| 3 | 2 | 4,5,6,7,8 | None |
| 4 | 3 | 9 | 5,6,7,8 |
| 5 | 3 | 9 | 4,6,7,8 |
| 6 | 3 | 9 | 4,5,7,8 |
| 7 | 3 | 9 | 4,5,6,8 |
| 8 | 3 | 9 | 4,5,6,7 |
| 9 | 4,5,6,7,8 | None | None |

---

## TODOs

- [x] 1. Create extern directory and symlink to evr-test-harness

  **What to do**:
  - Create `extern/` directory if it doesn't exist
  - Create symlink: `extern/evr-test-harness` → `~/src/evr-test-harness`
  - Verify symlink resolves correctly
  - Add `extern/evr-test-harness` to `.gitignore` (symlinks shouldn't be committed)

  **Must NOT do**:
  - Do NOT use absolute path in any committed files
  - Do NOT create the symlink if target doesn't exist

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Simple filesystem operation, single-step task
  - **Skills**: [`git-master`]
    - `git-master`: Needed for .gitignore modification

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (foundation)
  - **Blocks**: Task 2
  - **Blocked By**: None

  **References**:
  - `extern/` directory pattern - existing external dependencies location
  - `.gitignore` - existing ignore patterns to follow

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Symlink created and resolves
    Tool: Bash
    Preconditions: ~/src/evr-test-harness exists with pkg/testutil/
    Steps:
      1. ls -la extern/evr-test-harness
      2. Assert: output shows symlink pointing to /home/andrew/src/evr-test-harness
      3. ls extern/evr-test-harness/pkg/testutil/
      4. Assert: lists .go files (confirms resolution works)
    Expected Result: Symlink exists and points to valid directory
    Evidence: ls output captured

  Scenario: Symlink is gitignored
    Tool: Bash
    Preconditions: .gitignore exists
    Steps:
      1. grep "extern/evr-test-harness" .gitignore
      2. Assert: exit code 0 (pattern found)
    Expected Result: Symlink path is in .gitignore
    Evidence: grep output
  ```

  **Commit**: YES
  - Message: `chore: add evr-test-harness symlink for system tests`
  - Files: `.gitignore`
  - Pre-commit: `test -L extern/evr-test-harness`

---

- [x] 2. Initialize Go module for system tests

  **What to do**:
  - Create `tests/system/` directory
  - Initialize Go module: `go mod init github.com/EchoTools/nevr-runtime/tests/system`
  - Add `replace` directive for evr-test-harness: `replace github.com/EchoTools/evr-test-harness => ../../extern/evr-test-harness`
  - Add required dependencies (testify)
  - Run `go mod tidy` to verify

  **Must NOT do**:
  - Do NOT use absolute paths in go.mod
  - Do NOT import evr-test-harness before adding replace directive

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Standard Go module initialization
  - **Skills**: []
    - No special skills needed

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (foundation)
  - **Blocks**: Task 3
  - **Blocked By**: Task 1

  **References**:
  - `~/src/evr-test-harness/go.mod` - reference module name and Go version
  - `~/src/evr-test-harness/tests/integration/` - test patterns to follow

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Go module initializes and resolves dependencies
    Tool: Bash
    Preconditions: Task 1 complete, symlink exists
    Steps:
      1. cd tests/system && cat go.mod
      2. Assert: contains "module github.com/EchoTools/nevr-runtime/tests/system"
      3. Assert: contains "replace github.com/EchoTools/evr-test-harness"
      4. cd tests/system && go mod tidy
      5. Assert: exit code 0
      6. cat tests/system/go.sum
      7. Assert: file exists (dependencies resolved)
    Expected Result: Go module valid with working replace directive
    Evidence: go.mod and go.sum contents

  Scenario: Dependency resolution fails gracefully if harness not refactored
    Tool: Bash
    Preconditions: Symlink exists but pkg/testutil/ might not
    Steps:
      1. ls extern/evr-test-harness/pkg/testutil/ 2>&1
      2. If fails: output clear error message about prerequisite
    Expected Result: Clear error if prerequisite not met
    Evidence: Error message captured
  ```

  **Commit**: YES
  - Message: `chore(tests): initialize Go module for system tests`
  - Files: `tests/system/go.mod`, `tests/system/go.sum`
  - Pre-commit: `cd tests/system && go mod verify`

---

- [x] 3. Create test helpers and verify evr-test-harness imports

  **What to do**:
  - Create `tests/system/helpers_test.go` with:
    - Package declaration: `package system`
    - Import evr-test-harness/pkg/testutil
    - Define environment variable constants for paths
    - Create helper function to get DLL paths from build output
    - Create helper to deploy DLLs to game directory
  - Verify imports compile successfully

  **Must NOT do**:
  - Do NOT hardcode paths - use environment variables
  - Do NOT duplicate TestFixture/MCPClient code (import from harness)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Standard Go code, some domain knowledge needed
  - **Skills**: []
    - No special skills needed

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (foundation)
  - **Blocks**: Tasks 4, 5, 6, 7, 8
  - **Blocked By**: Task 2

  **References**:
  - `~/src/evr-test-harness/pkg/testutil/` - TestFixture and MCPClient interfaces
  - `~/src/evr-test-harness/tests/integration/main_test.go` - patterns for test setup
  - `test-headless-server.sh` - DLL deployment pattern (copy to game dir)
  - `dist/` - built DLL output location in nevr-runtime

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Helper file compiles with evr-test-harness imports
    Tool: Bash
    Preconditions: Task 2 complete
    Steps:
      1. cd tests/system && go build ./...
      2. Assert: exit code 0
      3. grep "testutil" tests/system/helpers_test.go
      4. Assert: import statement present
    Expected Result: Code compiles with external import
    Evidence: Build output

  Scenario: Helper provides DLL path resolution
    Tool: Bash
    Preconditions: helpers_test.go exists
    Steps:
      1. grep -E "func.*(DLL|Dll|dll)" tests/system/helpers_test.go
      2. Assert: at least one DLL-related helper function exists
    Expected Result: DLL path helpers defined
    Evidence: grep output
  ```

  **Commit**: YES
  - Message: `feat(tests): add system test helpers with evr-test-harness integration`
  - Files: `tests/system/helpers_test.go`
  - Pre-commit: `cd tests/system && go build ./...`

---

- [x] 4. Implement DLL loading/injection tests

  **What to do**:
  - Create `tests/system/dll_test.go`
  - Implement tests for:
    - Game starts without nevr-runtime DLLs (baseline)
    - Game loads with gamepatches.dll injected
    - Game loads with gameserver.dll injected
    - Game loads with telemetryagent.dll injected
    - All DLLs load together without conflicts
  - Use `testing.Short()` to skip slow tests
  - Use TestFixture for game lifecycle management

  **Must NOT do**:
  - Do NOT test DLL functionality here (just loading)
  - Do NOT leave game processes running after test

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Domain-specific test logic, moderate complexity
  - **Skills**: []
    - No special skills needed

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 5, 6, 7, 8)
  - **Blocks**: Task 9
  - **Blocked By**: Task 3

  **References**:
  - `test-headless-server.sh:10-20` - DLL copy pattern to game directory
  - `~/src/evr-test-harness/tests/integration/session_test.go` - session start/stop pattern
  - `dist/` - DLL output locations (gamepatches.dll, gameserver.dll, telemetryagent.dll)
  - `echovr/bin/win10/` - game directory where DLLs need to be deployed

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: DLL loading test compiles and has test functions
    Tool: Bash
    Preconditions: Task 3 complete
    Steps:
      1. cd tests/system && go test -list ".*" ./... 2>&1 | grep -i dll
      2. Assert: at least 3 test functions listed
    Expected Result: Multiple DLL tests defined
    Evidence: Test list output

  Scenario: DLL tests run in short mode (skip actual game)
    Tool: Bash
    Preconditions: dll_test.go exists
    Steps:
      1. cd tests/system && go test -v -short -run ".*DLL.*" ./... 2>&1
      2. Assert: output contains "SKIP" for integration tests
      3. Assert: exit code 0
    Expected Result: Tests skip gracefully in short mode
    Evidence: Test output with SKIP messages

  Scenario: DLL test actually loads game (full mode, if harness available)
    Tool: Bash
    Preconditions: evr-test-harness built, game binary available
    Steps:
      1. cd tests/system && timeout 120 go test -v -run "TestDLLLoading" ./... 2>&1 || true
      2. If success: Assert output contains "game started"
      3. If skip: Assert output contains "SKIP" (acceptable)
    Expected Result: Test either runs successfully or skips with clear message
    Evidence: Full test output
  ```

  **Commit**: YES (groups with Tasks 5-8)
  - Message: `feat(tests): add DLL loading system tests`
  - Files: `tests/system/dll_test.go`
  - Pre-commit: `cd tests/system && go build ./...`

---

- [x] 5. Implement game patch behavior tests

  **What to do**:
  - Create `tests/system/patches_test.go`
  - Implement tests for:
    - Headless mode (`-headless` flag) - game runs without window
    - Server mode (`-server` flag) - game runs as dedicated server
    - noOVR mode (`-noovr` flag) - game runs without VR requirement
    - Windowed mode (`-windowed` flag) - game runs in window
    - CLI flag combinations work together
  - Verify patch effects via game state HTTP API
  - Use `testing.Short()` for long tests

  **Must NOT do**:
  - Do NOT test multiplayer behavior here (separate test file)
  - Do NOT modify game patch code

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Domain-specific testing, requires understanding game flags
  - **Skills**: []
    - No special skills needed

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 4, 6, 7, 8)
  - **Blocks**: Task 9
  - **Blocked By**: Task 3

  **References**:
  - `src/gamepatches/patches.cpp` - CLI flag definitions and behavior
  - `~/src/evr-test-harness/internal/echovr/process.go:70-100` - game launch options
  - `test-headless-server.sh` - example of headless mode testing
  - `~/src/evr-test-harness/tests/integration/state_test.go` - state query patterns

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Patch tests exist for each CLI flag
    Tool: Bash
    Preconditions: Task 3 complete
    Steps:
      1. grep -c "func Test" tests/system/patches_test.go
      2. Assert: at least 4 test functions (one per flag)
      3. grep -E "(headless|server|noovr|windowed)" tests/system/patches_test.go
      4. Assert: all 4 flags mentioned in tests
    Expected Result: Tests cover all major patch flags
    Evidence: grep output showing test coverage

  Scenario: Headless mode test verifies no window
    Tool: Bash (examining test code)
    Preconditions: patches_test.go exists
    Steps:
      1. grep -A 20 "TestHeadlessMode" tests/system/patches_test.go
      2. Assert: test checks for headless state via API or process
    Expected Result: Test has actual verification logic
    Evidence: Test implementation excerpt
  ```

  **Commit**: YES (groups with Tasks 4, 6-8)
  - Message: `feat(tests): add game patch behavior system tests`
  - Files: `tests/system/patches_test.go`
  - Pre-commit: `cd tests/system && go build ./...`

---

- [x] 6. Implement multiplayer functionality tests

  **What to do**:
  - Create `tests/system/multiplayer_test.go`
  - Implement tests for:
    - Session creation (server starts and accepts connections)
    - Player join event detection
    - Player leave event detection
    - Match state transitions
    - Multi-instance coordination (2 game instances)
  - Use evr-test-harness event streaming for verification
  - Use `testing.Short()` for multi-instance tests

  **Must NOT do**:
  - Do NOT test without Nakama backend running
  - Do NOT exceed port range 6731-6750 for multi-instance

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Complex coordination between multiple game instances
  - **Skills**: []
    - No special skills needed

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 4, 5, 7, 8)
  - **Blocks**: Task 9
  - **Blocked By**: Task 3

  **References**:
  - `~/src/evr-test-harness/tests/integration/multi_instance_test.go` - multi-instance patterns
  - `~/src/evr-test-harness/tests/integration/session_test.go` - session lifecycle
  - `~/src/evr-test-harness/internal/events/parser.go` - event types (player_join, player_leave, match_start)
  - `src/gameserver/` - multiplayer server implementation details

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Multiplayer test file has session and player tests
    Tool: Bash
    Preconditions: Task 3 complete
    Steps:
      1. grep -c "func Test" tests/system/multiplayer_test.go
      2. Assert: at least 3 test functions
      3. grep -E "(Session|Player|Match)" tests/system/multiplayer_test.go
      4. Assert: covers session, player, and match concepts
    Expected Result: Tests cover core multiplayer scenarios
    Evidence: grep output

  Scenario: Multi-instance test uses correct port range
    Tool: Bash
    Preconditions: multiplayer_test.go exists
    Steps:
      1. grep -E "673[0-9]" tests/system/multiplayer_test.go
      2. Assert: ports in 6731-6750 range used
    Expected Result: Tests use designated multi-instance port range
    Evidence: Port numbers in test code
  ```

  **Commit**: YES (groups with Tasks 4, 5, 7, 8)
  - Message: `feat(tests): add multiplayer functionality system tests`
  - Files: `tests/system/multiplayer_test.go`
  - Pre-commit: `cd tests/system && go build ./...`

---

- [x] 7. Implement telemetry streaming tests

  **What to do**:
  - Create `tests/system/telemetry_test.go`
  - Implement tests for:
    - TelemetryAgent DLL loads and initializes
    - Frame data is captured from game state
    - Telemetry is streamed to external endpoint (mock or real)
    - Telemetry format matches expected schema
  - Use game state HTTP API to correlate telemetry with actual state
  - Use `testing.Short()` for streaming tests

  **Must NOT do**:
  - Do NOT test without game running
  - Do NOT send telemetry to production endpoints

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Moderate complexity, focused domain
  - **Skills**: []
    - No special skills needed

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 4, 5, 6, 8)
  - **Blocks**: Task 9
  - **Blocked By**: Task 3

  **References**:
  - `src/telemetryagent/` - TelemetryAgent implementation
  - `src/telemetryagent/frame_processor.cpp` - frame processing logic
  - `~/src/evr-test-harness/internal/echovr/state.go` - game state query API
  - `~/src/evr-test-harness/tests/integration/state_test.go` - state query patterns

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Telemetry test file covers agent lifecycle
    Tool: Bash
    Preconditions: Task 3 complete
    Steps:
      1. grep -c "func Test" tests/system/telemetry_test.go
      2. Assert: at least 2 test functions
      3. grep -E "(Telemetry|Frame|Stream)" tests/system/telemetry_test.go
      4. Assert: covers telemetry concepts
    Expected Result: Tests cover telemetry agent functionality
    Evidence: grep output

  Scenario: Tests don't use production endpoints
    Tool: Bash
    Preconditions: telemetry_test.go exists
    Steps:
      1. grep -v "localhost\|127.0.0.1\|mock\|test" tests/system/telemetry_test.go | grep -E "https?://" || echo "PASS: no external URLs"
      2. Assert: no production URLs found
    Expected Result: Only test/mock endpoints used
    Evidence: grep output
  ```

  **Commit**: YES (groups with Tasks 4-6, 8)
  - Message: `feat(tests): add telemetry streaming system tests`
  - Files: `tests/system/telemetry_test.go`
  - Pre-commit: `cd tests/system && go build ./...`

---

- [x] 8. Implement full E2E integration tests

  **What to do**:
  - Create `tests/system/e2e_test.go`
  - Implement tests for complete cycle:
    - Build → Deploy → Start → Verify → Stop
    - Fresh build DLLs are deployed to game directory
    - Game starts with all DLLs loaded
    - Game reaches ready state (via HTTP API)
    - Events are streaming (via event API)
    - Clean shutdown with no orphaned processes
  - Use `testing.Short()` to skip by default

  **Must NOT do**:
  - Do NOT run build in test (assume pre-built DLLs)
  - Do NOT leave any processes running

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Complex end-to-end orchestration
  - **Skills**: []
    - No special skills needed

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 4, 5, 6, 7)
  - **Blocks**: Task 9
  - **Blocked By**: Task 3

  **References**:
  - `test-headless-server.sh` - current manual E2E pattern
  - `~/src/evr-test-harness/tests/integration/` - comprehensive test patterns
  - All previous test files - combine patterns from each

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: E2E test covers full lifecycle
    Tool: Bash
    Preconditions: Task 3 complete
    Steps:
      1. grep -E "(Deploy|Start|Stop|Verify)" tests/system/e2e_test.go
      2. Assert: all lifecycle phases mentioned
      3. grep "defer" tests/system/e2e_test.go
      4. Assert: cleanup defers present
    Expected Result: Test covers deploy → start → verify → stop
    Evidence: grep output

  Scenario: E2E test skips by default in short mode
    Tool: Bash
    Preconditions: e2e_test.go exists
    Steps:
      1. grep "testing.Short()" tests/system/e2e_test.go
      2. Assert: short mode check present
    Expected Result: Long tests are skippable
    Evidence: grep output
  ```

  **Commit**: YES (groups with Tasks 4-7)
  - Message: `feat(tests): add full E2E integration system tests`
  - Files: `tests/system/e2e_test.go`
  - Pre-commit: `cd tests/system && go build ./...`

---

- [x] 9. Add Makefile targets and documentation

  **What to do**:
  - Add Makefile targets:
    - `test-system`: Run all system tests (full mode)
    - `test-system-short`: Run quick tests only (short mode)
    - `test-system-dll`: Run only DLL tests
    - `test-system-verbose`: Run with verbose output
  - Create `tests/system/README.md` with:
    - Prerequisites (evr-test-harness setup, built DLLs)
    - How to run tests
    - Environment variables
    - Troubleshooting guide
  - Verify targets work correctly

  **Must NOT do**:
  - Do NOT add CI/CD integration
  - Do NOT modify existing Makefile targets

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Simple file additions, documentation
  - **Skills**: [`git-master`]
    - `git-master`: For clean commit of Makefile changes

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 3 (final)
  - **Blocks**: None (final task)
  - **Blocked By**: Tasks 4, 5, 6, 7, 8

  **References**:
  - `Makefile` - existing target patterns
  - `~/src/evr-test-harness/Makefile` - test target patterns
  - `~/src/evr-test-harness/README.md` - documentation style

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Makefile has all required targets
    Tool: Bash
    Preconditions: Tasks 4-8 complete
    Steps:
      1. grep "test-system:" Makefile
      2. Assert: target exists
      3. grep "test-system-short:" Makefile
      4. Assert: target exists
      5. make test-system-short 2>&1 | head -20
      6. Assert: tests attempt to run (may skip in short mode)
    Expected Result: Makefile targets defined and functional
    Evidence: Makefile excerpts and make output

  Scenario: README documents prerequisites
    Tool: Bash
    Preconditions: README exists
    Steps:
      1. grep -i "prerequisite\|require" tests/system/README.md
      2. Assert: prerequisites section exists
      3. grep "evr-test-harness" tests/system/README.md
      4. Assert: harness dependency mentioned
    Expected Result: Documentation covers setup requirements
    Evidence: README excerpts
  ```

  **Commit**: YES
  - Message: `docs(tests): add system test Makefile targets and documentation`
  - Files: `Makefile`, `tests/system/README.md`
  - Pre-commit: `make test-system-short`

---

## Commit Strategy

| After Task | Message | Files | Verification |
|------------|---------|-------|--------------|
| 1 | `chore: add evr-test-harness symlink for system tests` | .gitignore | `test -L extern/evr-test-harness` |
| 2 | `chore(tests): initialize Go module for system tests` | tests/system/go.mod, go.sum | `cd tests/system && go mod verify` |
| 3 | `feat(tests): add system test helpers with evr-test-harness integration` | tests/system/helpers_test.go | `cd tests/system && go build ./...` |
| 4-8 | `feat(tests): add system tests for DLL, patches, multiplayer, telemetry, E2E` | tests/system/*_test.go | `cd tests/system && go build ./...` |
| 9 | `docs(tests): add system test Makefile targets and documentation` | Makefile, tests/system/README.md | `make test-system-short` |

---

## Success Criteria

### Verification Commands
```bash
# Symlink works
test -L extern/evr-test-harness && echo "PASS: symlink exists"

# Go module resolves
cd tests/system && go mod tidy && echo "PASS: module valid"

# All test files compile
cd tests/system && go build ./... && echo "PASS: tests compile"

# Short mode runs without errors
cd tests/system && go test -v -short ./... && echo "PASS: short tests pass"

# Makefile targets work
make test-system-short && echo "PASS: make target works"

# At least 5 test functions (one per area)
cd tests/system && go test -list ".*" ./... 2>&1 | grep -c "^Test" | xargs test 5 -le && echo "PASS: sufficient tests"
```

### Final Checklist
- [x] Symlink `extern/evr-test-harness` exists and resolves
- [x] `tests/system/go.mod` has correct replace directive
- [x] All 5 test files exist (dll, patches, multiplayer, telemetry, e2e)
- [x] All test files compile without errors
- [x] `make test-system-short` executes successfully
- [x] `tests/system/README.md` documents workflow
- [x] No hardcoded absolute paths in Go code
- [x] No CI/CD files modified

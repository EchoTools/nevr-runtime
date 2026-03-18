# Draft: evr-test-harness Integration

## Context

**nevr-runtime**: C++ DLL-based runtime patches project
- Components: gameserver, gamepatches, telemetryagent, common
- Build: CMake + vcpkg + MinGW (cross-compiled via Wine)
- Current Tests: Manual shell scripts only (test-headless-server.sh, test-client.sh)
- CI/CD: GitHub Actions (build only, no tests)

**evr-test-harness**: Go-based MCP test server at ~/src/evr-test-harness
- Protocol: MCP (Model Context Protocol) via stdio
- Purpose: AI-driven game testing harness for Echo VR
- Tools: 21 MCP tools for game control, state queries, events, screenshots, debugging
- Tests: 44 integration tests, all passing
- Capabilities:
  - Process management (start/stop Echo VR via Wine)
  - Multi-instance support (ports 6721-6790)
  - Game state monitoring via HTTP API
  - Event streaming (13 event types)
  - Input simulation (xdotool)
  - Screenshot capture
  - Nakama backend control
  - Debugging (winedbg/GDB)

## Requirements (confirmed)

- [x] Integration approach: **Symlink** - `evr-test-harness` symlinked into nevr-runtime
- [x] What to test: **Everything**
  - DLL loading/injection into Echo VR
  - Game behavior changes (patches: headless, server mode, CLI flags)
  - Multiplayer functionality (sessions, player joins, events)
  - Telemetry streaming (TelemetryAgent capturing/streaming game state)
  - Full integration cycle (build → inject → run → verify)
- [x] CI/CD integration: **No** - local development tool only
- [x] Test execution workflow: **Go tests** using evr-test-harness's MCP client library
- [x] Test structure/location: **tests/system/** directory

## Technical Decisions

- **Symlink location**: `./extern/evr-test-harness` → `~/src/evr-test-harness`
- **Test runner**: Go tests (same pattern as evr-test-harness)
- **Test structure**: `tests/system/` directory with Go test files
- **Test invocation**: Use MCP client to communicate with harness
- **Build integration**: Build-then-test workflow
- **Test organization**: Per-area test files (dll_test.go, patches_test.go, multiplayer_test.go, telemetry_test.go, e2e_test.go)
- **Go module**: Sub-module at `tests/system/go.mod` importing evr-test-harness
- **MCPClient strategy**: Refactor evr-test-harness first to export TestFixture/MCPClient as `pkg/testutil/` library

## Two-Phase Approach

**Phase 1 (evr-test-harness):**
- Create `pkg/testutil/` package with exported TestFixture, MCPClient
- Move/refactor code from `tests/integration/main_test.go`
- Update existing integration tests to use new package
- Ensure tests still pass

**Phase 2 (nevr-runtime):**
- Create symlink to evr-test-harness
- Set up tests/system/ with go.mod
- Import evr-test-harness/pkg/testutil
- Write test files per area
- Add Makefile targets

## Scope Boundaries

**IN SCOPE (this plan - nevr-runtime):**
- Symlink setup: ./extern/evr-test-harness → ~/src/evr-test-harness
- Test infrastructure: tests/system/ with go.mod
- Import evr-test-harness/pkg/testutil (assumes refactoring is done)
- Test files for all 5 areas
- Makefile targets for running tests
- Documentation for test workflow

**OUT OF SCOPE (separate plan - evr-test-harness):**
- Refactoring evr-test-harness to export pkg/testutil/ ← PREREQUISITE
- Changes to evr-test-harness itself

**PREREQUISITES (must be done first):**
- evr-test-harness must export `pkg/testutil/` with TestFixture, MCPClient
- Run separate planning session in ~/src/evr-test-harness for that work

**ALSO OUT OF SCOPE:**
- CI/CD automation
- Unit tests for C++ components (separate concern)

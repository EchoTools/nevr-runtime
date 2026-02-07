# System Tests for NEVR Server

This directory contains automated integration tests for the NEVR Server and its DLL components using the `evr-test-harness` testing framework.

## Overview

The system test suite verifies:

- **DLL Loading** - Deployment and loading of gamepatches.dll, gameserver.dll, and telemetryagent.dll
- **Game Patches** - Behavior of CLI flags (headless mode, server mode, etc.)
- **Multiplayer Functionality** - Session creation, player events, match state
- **Telemetry** - Frame capture and HTTP polling functionality
- **End-to-End Integration** - Full lifecycle with all DLLs loaded together

### Test Files

| File | Tests | Purpose |
|------|-------|---------|
| `dll_test.go` | 5 | DLL deployment and loading |
| `patches_test.go` | 7 | Game patch flag behavior |
| `multiplayer_test.go` | 5 | Multiplayer session management |
| `telemetry_test.go` | 6 | Telemetry agent functionality |
| `e2e_test.go` | 4 | End-to-end integration tests |

**Total: 27 test functions**

## Prerequisites

Before running system tests, ensure:

### 1. evr-test-harness Symlink
The `evr-test-harness` project must be symlinked at `extern/evr-test-harness`:

```bash
# Create symlink (one-time setup)
ln -s ~/src/evr-test-harness extern/evr-test-harness
```

This symlink is required for:
- TestFixture: Game process management via MCP protocol
- Test utilities: HTTP verification, process assertions
- MCP client: Communication with evr-test-harness MCP server

### 2. Built DLL Files
All NEVR Server DLLs must be built before running tests:

```bash
# Build all DLLs
make build

# Or build distribution with renamed DLLs
make dist
```

Tests look for DLLs in:
- **Primary**: `../../dist/` (distribution packages)
- **Default**: `../../build/mingw-release/bin/` (MinGW builds)
- **Configurable**: Use `NEVR_BUILD_DIR` environment variable

Expected DLL files:
- `gamepatches.dll`
- `gameserver.dll`
- `telemetryagent.dll`
- `gamepatcheslegacy.dll`
- `gameserverlegacy.dll`

### 3. Echo VR Game Installation
A working Echo VR installation is required for full integration tests.

Default location: `../../ready-at-dawn-echo-arena/`
Configurable via: `EVR_GAME_DIR` environment variable

Required subdirectories:
- `bin/win10/` - DLL deployment target
- Game executable accessible to evr-test-harness

### 4. Go Module Dependencies
Dependencies are automatically resolved via `go.mod`:

```bash
# Verify dependencies
cd tests/system && go mod verify

# Install dependencies
cd tests/system && go mod download
```

Key dependencies:
- `testify` - Assertion and mocking framework
- `evr-test-harness` - Test utilities (via symlink)

## Environment Variables

### NEVR_BUILD_DIR
**Default**: `../../dist`

Directory containing built NEVR Server DLLs.

```bash
# Override to use custom build location
export NEVR_BUILD_DIR=/path/to/custom/build
go test -v ./...
```

### EVR_GAME_DIR
**Default**: `../../ready-at-dawn-echo-arena`

Path to Echo VR game installation directory.

```bash
# Override to use custom game location
export EVR_GAME_DIR=/path/to/echo-arena
go test -v ./...
```

## Running Tests

### Using Make Targets (Recommended)

```bash
# Run all tests (except slow E2E tests)
make test-system-short

# Run all tests including full E2E
make test-system

# Run only DLL loading tests
make test-system-dll

# Run with verbose output and no caching
make test-system-verbose
```

### Using go test Directly

```bash
# Change to test directory
cd tests/system

# Run all tests (full mode)
go test -v ./...

# Run quick tests only (skip long-running tests)
go test -v -short ./...

# Run specific test
go test -v -run TestDLLLoading_Gamepatches ./...

# Run tests matching pattern
go test -v -run ".*DLL.*" ./...

# Run with no test caching
go test -v -count=1 ./...

# Set environment variables
export NEVR_BUILD_DIR=/custom/path
export EVR_GAME_DIR=/custom/game
go test -v ./...
```

### Running Individual Test Types

```bash
# DLL loading tests only
go test -v -short -run ".*DLL.*" ./...

# Game patch tests
go test -v -short -run ".*Patches.*" ./...

# Multiplayer tests
go test -v -short -run ".*Multiplayer.*" ./...

# Telemetry tests
go test -v -short -run ".*Telemetry.*" ./...

# End-to-end integration tests
go test -v -run ".*E2E.*" ./...
```

## Test Structure

### DLL Loading Tests (dll_test.go)

Tests basic DLL deployment without requiring a running game instance:

1. **TestDLLLoading_Gamepatches** - Deploy gamepatches.dll
2. **TestDLLLoading_Gameserver** - Deploy gameserver.dll
3. **TestDLLLoading_Telemetryagent** - Deploy telemetryagent.dll
4. **TestDLLLoading_AllDLLs** - Deploy all DLLs together
5. **TestDLLLoading_CleanupRemovesAllDLLs** - Verify cleanup works

**Run with**: `make test-system-short`
**Skip in short mode**: No (always runs)
**Requires**: Built DLLs, game directory with `bin/win10/` subdirectory

### Game Patch Tests (patches_test.go)

Tests gamepatches.dll CLI flag behaviors:

1. **TestPatches_HeadlessMode** - `-headless` flag (no window, no audio)
2. **TestPatches_ServerMode** - `-server` flag (dedicated server)
3. **TestPatches_NoOVRMode** - `-noovr` flag (no VR required)
4. **TestPatches_WindowedMode** - `-windowed` flag (desktop window)
5. **TestPatches_FlagCombinations** - Multiple flags together
6. **TestPatches_InvalidFlagCombinations** - Mutually exclusive flags
7. **TestPatches_TimestepConfiguration** - `-timestep` flag

**Run with**: `go test -v -short -run ".*Patches.*" ./...`
**Skip in short mode**: Yes (skips with "skipping integration test in short mode")
**Requires**: Game binary, evr-test-harness MCP server

### Multiplayer Tests (multiplayer_test.go)

Tests gameserver.dll multiplayer functionality:

1. **TestMultiplayer_SessionCreation** - Create game session via HTTP API
2. **TestMultiplayer_PlayerEvents** - Player join/leave event handling
3. **TestMultiplayer_MatchState** - Match state transitions
4. **TestMultiplayer_MultiInstance** - Coordinate multiple game instances
5. **TestMultiplayer_SessionCleanup** - Proper session shutdown

**Run with**: `go test -v -short -run ".*Multiplayer.*" ./...`
**Skip in short mode**: Yes (resource-intensive tests skip)
**Requires**: Game binary, multiple available ports (6731-6750)

### Telemetry Tests (telemetry_test.go)

Tests telemetryagent.dll functionality:

1. **TestTelemetry_AgentLoads** - DLL loads into game process
2. **TestTelemetry_FrameCapture** - Frame capture via HTTP API
3. **TestTelemetry_HTTPPollerDirectQuery** - HTTP endpoint validation
4. **TestTelemetry_Streaming** - Streaming to external endpoint (skipped)
5. **TestTelemetry_SchemaValidation** - Protobuf schema validation (skipped)
6. **TestTelemetry_AgentInitialization** - C API testing (skipped)

**Run with**: `go test -v -short -run ".*Telemetry.*" ./...`
**Skip in short mode**: Yes (full integration skipped)
**Requires**: Game binary, HTTP endpoint for telemetry

### End-to-End Tests (e2e_test.go)

Tests full integration with all DLLs:

1. **TestE2E_FullCycle** - Complete lifecycle (deploy → start → stop → cleanup)
2. **TestE2E_AllDLLsLoaded** - All DLLs load together
3. **TestE2E_EventStreaming** - Continuous event streaming
4. **TestE2E_CleanShutdownNoOrphans** - Proper process cleanup

**Run with**: `go test -v -run ".*E2E.*" ./...`
**Skip in short mode**: Yes (resource-intensive, 2-3 minutes each)
**Requires**: All prerequisites, 30-60 minutes total runtime

## HTTP API Endpoints

The game provides HTTP API endpoints that tests query:

### Game Session Endpoint

**URL**: `http://localhost:PORT/session`

Returns current game state:

```json
{
  "sessionid": "abc-123",
  "game_status": "playing",
  "game_clock_display": "12:34",
  "players": []
}
```

### Player Bones Endpoint

**URL**: `http://localhost:PORT/player_bones`

Returns player skeleton positions and tracking data.

**Port Assignment**:
- Session Creation: 6731
- Player Events: 6732
- Match State: 6733
- Multi-Instance Server: 6734
- Multi-Instance Client: 6735
- Session Cleanup: 6736
- E2E Tests: 6796-6799

## Troubleshooting

### DLL Not Found

```
Error: gamepatches.dll not found at ../../dist/gamepatches.dll
```

**Solutions**:
1. Build DLLs: `make build`
2. Check build directory: `ls ../../build/mingw-release/bin/`
3. Set custom path: `export NEVR_BUILD_DIR=/path/to/dlls`

### Game Directory Not Found

```
Error: game directory not found at ../../ready-at-dawn-echo-arena
```

**Solutions**:
1. Install Echo VR game
2. Symlink to custom location: `ln -s /actual/path ready-at-dawn-echo-arena`
3. Set environment variable: `export EVR_GAME_DIR=/path/to/echo-arena`

### evr-test-harness Symlink Missing

```
Error: package github.com/EchoTools/evr-test-harness: unrecognized import path
```

**Solution**:
```bash
ln -s ~/src/evr-test-harness extern/evr-test-harness
go mod verify
```

### Test Hangs or Times Out

**Symptoms**: Test runs but never completes (>120 seconds)

**Causes**:
- Game process not starting properly (Wine/Proton issue)
- MCP server not responding
- Game infinite loop or deadlock

**Solutions**:
1. Check game binary permissions: `ls -la ../../ready-at-dawn-echo-arena/EchoVR`
2. Verify Wine is installed: `wine --version`
3. Run test with timeout: `timeout 30 go test -run TestE2E_FullCycle ./...`
4. Kill orphaned processes: `killall EchoVR.exe; killall wine`

### HTTP API Not Responding

```
Error: failed to connect to http://localhost:6731/session
```

**Causes**:
- Game process not reached ready state
- Port already in use
- Game crashed immediately after start

**Solutions**:
1. Verify port available: `netstat -tuln | grep 6731`
2. Check game process: `ps aux | grep EchoVR`
3. Examine game logs: Check `EchoVR.exe` process output
4. Increase wait timeout: Modify test timeout values

### Permission Denied on bin/win10

```
Error: failed to copy gamepatches.dll: permission denied
```

**Cause**: Game directory not writable

**Solutions**:
1. Check permissions: `ls -ld ../../ready-at-dawn-echo-arena/bin/win10/`
2. Make writable: `chmod -R u+w ../../ready-at-dawn-echo-arena/`
3. Run with appropriate permissions: `sudo go test` (if necessary)

### Go Module Issues

```
Error: go: unrecognized import path
```

**Solution**:
```bash
cd tests/system
go mod tidy
go mod verify
```

### Test Skipping Unexpectedly

```
--- SKIP: TestPatches_HeadlessMode (0.00s)
    patches_test.go:15: skipping integration test in short mode
```

**Cause**: Running with `-short` flag (intentional skip)

**Solution**: Remove `-short` flag to run full tests:
```bash
go test -v ./...          # Full tests
go test -v -short ./...   # Quick tests only
```

## Development Notes

### Writing New Tests

All new system tests should:

1. **Skip in short mode**:
   ```go
   if testing.Short() {
       t.Skip("skipping integration test in short mode")
   }
   ```

2. **Deploy DLLs before use**:
   ```go
   deployAllDLLs(t)
   defer cleanupAllDLLs(t)
   ```

3. **Use TestFixture for game control**:
   ```go
   f := testutil.NewFixture(t)
   defer f.Cleanup()
   ```

4. **Assign unique ports** to tests to enable parallel execution

5. **Implement proper cleanup** with defer statements

### Debugging Tips

- Add `t.Logf()` calls for debugging: `go test -v -run TestName ./...`
- Inspect helper functions in `helpers_test.go`
- Check evr-test-harness MCP server logs
- Verify DLLs exist before test: `ls -la ../../dist/gamepatches.dll`
- Kill orphaned processes: `killall EchoVR.exe wine wineserver`

### Performance Considerations

- **Short mode tests**: ~10 seconds (skips integration)
- **DLL tests**: ~30 seconds (no game instance)
- **Patch tests**: ~2-3 minutes per test
- **Multiplayer tests**: ~2-3 minutes per test
- **E2E tests**: ~2-3 minutes per test (8-12 minutes total)
- **Full suite**: ~15-20 minutes with all tests

## Building and Running from CI/CD

### GitHub Actions Example

```yaml
- name: Build NEVR Server
  run: make build

- name: Run quick system tests
  run: make test-system-short

- name: Run full system tests
  run: make test-system
  if: github.event_name == 'push'
```

### Environment Setup in CI

```bash
# Install dependencies
go mod download

# Set paths if non-standard
export NEVR_BUILD_DIR=${{ github.workspace }}/dist
export EVR_GAME_DIR=${{ github.workspace }}/ready-at-dawn-echo-arena

# Run tests
make test-system-short
```

## References

- **Makefile**: `/home/andrew/src/nevr-server/Makefile` - Build targets
- **evr-test-harness**: `/home/andrew/src/evr-test-harness/` - Test framework
- **Test Helpers**: `helpers_test.go` - DLL deployment utilities
- **NEVR Server README**: `../../README.md` - Project overview

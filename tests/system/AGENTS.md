# tests/system/

**Generated:** 2026-02-09  
**Commit:** abc5734  
**Branch:** build/mingw-minhook

## OVERVIEW

Go test suite (Go 1.20+) for end-to-end testing of gamepatches, gameserver, telemetryagent via MCP protocol. Supports `-short` mode to skip multiplayer/e2e tests requiring live game clients.

## STRUCTURE

```
README.md                 # Test overview, -short flag usage
dll_test.go               # DLL loading, injection verification
gamepatches_test.go       # CLI flag patching, headless mode tests
gameserver_test.go        # ServerContext state machine, message encoding
telemetry_test.go         # Frame polling, event detection
multiplayer_test.go       # Two-client scenarios (skipped with -short)
e2e_test.go               # Full match simulation (skipped with -short)
mcp_client.go             # MCP protocol client (src/evr_mcp integration)
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Test categories | `README.md` line 12 | DLL, patches, server, telemetry, multiplayer, e2e |
| Short mode logic | `dll_test.go` line 8 | `if testing.Short() { t.Skip(...) }` |
| MCP protocol client | `mcp_client.go` line 45 | Wraps src/evr_mcp server calls |
| State machine tests | `gameserver_test.go` line 123 | Lobby → InMatch → PostGame transitions |
| Event detection tests | `telemetry_test.go` line 89 | GoalScored, ThrowSuccess, Save events |
| DLL injection verification | `dll_test.go` line 67 | Checks dbgcore.dll, pnsradgameserver.dll loaded |

## CONVENTIONS

- **Go 1.20+ required** (uses generics, new testing.T methods)
- **`-short` flag** (skips multiplayer/e2e, runs in <30s)
- **MCP protocol integration** (src/evr_mcp provides test harness)
- **Table-driven tests** (subtests via `t.Run()`)
- **No external mocks** (uses real DLLs, live game client for full tests)

## ANTI-PATTERNS (THIS PROJECT)

| Forbidden | Why |
|-----------|-----|
| Tests without -short support | CI fails (no game client available) |
| Hardcoded game paths | Read from environment or MCP config |
| Mocking MCP protocol | Tests must use real src/evr_mcp server |
| Skipping cleanup (t.Cleanup) | Leaves DLLs loaded, breaks subsequent tests |
| Parallel tests without isolation | Game process is shared, state conflicts |

## BUILD & RUN

```bash
# Run all tests (requires live game client)
cd tests/system && go test -v

# Run short tests only (CI-safe, no game client)
go test -v -short

# Run specific test suite
go test -v -run TestGamePatches -short
```

## TEST CATEGORIES

- **DLL** (`dll_test.go`): ✅ Short mode, requires built DLLs
- **Patches** (`gamepatches_test.go`): ✅ Short mode, requires gamepatches.dll
- **Server** (`gameserver_test.go`): ✅ Short mode, requires gameserver.dll + MCP
- **Telemetry** (`telemetry_test.go`): ✅ Short mode, requires telemetryagent.dll
- **Multiplayer** (`multiplayer_test.go`): ❌ Skipped in short mode (2+ clients)
- **E2E** (`e2e_test.go`): ❌ Skipped in short mode (full match simulation)

## MCP PROTOCOL (mcp_client.go)

**Wraps src/evr_mcp server for test control**

- `StartGame("-headless", "-server")` - Launch game with DLLs
- `GetGameState()` - Poll current state
- `WaitForState("InMatch", 30*time.Second)` - Block until state
- `StopGame()` - Terminate gracefully (use in `defer` with `t.Cleanup`)

## SHORT MODE LOGIC

```go
if testing.Short() {
    t.Skip("Skipping multiplayer test in short mode")
}
```

**Use `-short`**: CI, quick validation, single-client tests only

## DEBUGGING

- **DLL not found**: Verify `build/*/bin/`, check MCP config paths
- **MCP connection failed**: Check src/evr_mcp running, verify HTTP port
- **Test timeout**: Use `-short` to skip slow tests, or increase `-timeout` flag
- **State transitions fail**: Check gameserver logs, verify message encoding

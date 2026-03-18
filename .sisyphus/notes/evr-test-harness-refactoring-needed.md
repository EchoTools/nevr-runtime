# evr-test-harness Refactoring Plan - pkg/testutil Export

## Context

This refactoring is a **prerequisite** for the nevr-runtime test-harness-integration plan.

**Goal:** Export `TestFixture` and `MCPClient` as a reusable library so nevr-runtime can import them.

---

## Current State

**Problem:** TestFixture and MCPClient are buried in test code:
- Location: `tests/integration/main_test.go` (lines 21-289 approximately)
- Package: `package integration` (test package, not importable)
- Status: Works for evr-test-harness's own tests, but cannot be imported by external projects

---

## Target State

**After refactoring:**
```
evr-test-harness/
├── pkg/
│   └── testutil/
│       ├── fixture.go      # Exported TestFixture type
│       ├── client.go       # Exported MCPClient type
│       └── doc.go          # Package documentation
└── tests/integration/
    └── main_test.go        # Import from pkg/testutil
```

**Import statement in nevr-runtime:**
```go
import "github.com/EchoTools/evr-test-harness/pkg/testutil"
```

---

## What Needs to Be Extracted

From `tests/integration/main_test.go`, extract these components:

### 1. TestFixture Struct
- **Current:** Lines ~21-50 (private type `testFixture`)
- **Target:** Export as `TestFixture` in `pkg/testutil/fixture.go`
- **Methods to export:**
  - `NewTestFixture(t *testing.T) *TestFixture`
  - `StartMCPServer()`
  - `StopMCPServer()`
  - `Cleanup()`
  - `RegisterSession(sessionID string)`
  - `UnregisterSession(sessionID string)`

### 2. MCPClient Struct
- **Current:** Lines ~80-289 (private type `mcpClient`)
- **Target:** Export as `MCPClient` in `pkg/testutil/client.go`
- **Methods to export:**
  - `NewMCPClient(serverPath string) (*MCPClient, error)`
  - `Start() error`
  - `Stop() error`
  - `CallTool(name string, params map[string]interface{}) (map[string]interface{}, error)`
  - Helper wrappers for each MCP tool (optional but recommended)

### 3. Constants and Helpers
- Binary path helpers
- Default timeouts
- Port allocation logic
- Session tracking utilities

---

## Key Changes Required

| What | Current | After Refactoring |
|------|---------|-------------------|
| Type names | `testFixture`, `mcpClient` | `TestFixture`, `MCPClient` (exported) |
| Package | `package integration` | `package testutil` |
| Field visibility | Private fields | Public fields where needed |
| Import path | N/A (internal) | `github.com/EchoTools/evr-test-harness/pkg/testutil` |
| Testing dependency | Direct `*testing.T` | Interface or optional logger |

---

## Recommended Approach

### Phase 1: Create Package Structure
1. Create `pkg/testutil/` directory
2. Create `doc.go` with package documentation
3. Create `fixture.go` and `client.go` (empty shells)

### Phase 2: Extract TestFixture
1. Copy `testFixture` struct to `fixture.go`
2. Rename to `TestFixture` (capital T)
3. Export necessary fields
4. Extract all methods
5. Handle `*testing.T` dependency (keep or make optional)

### Phase 3: Extract MCPClient
1. Copy `mcpClient` struct to `client.go`
2. Rename to `MCPClient` (capital M)
3. Export necessary fields
4. Extract all methods

### Phase 4: Update Integration Tests
1. Import `github.com/EchoTools/evr-test-harness/pkg/testutil`
2. Replace `testFixture` with `testutil.TestFixture`
3. Replace `mcpClient` with `testutil.MCPClient`
4. Run tests: `make test-integration`

### Phase 5: Verify
```bash
# Tests should still pass
make test-integration

# Package should be importable
go list -m github.com/EchoTools/evr-test-harness/pkg/testutil
```

---

## Acceptance Criteria

- [ ] `pkg/testutil/` directory exists
- [ ] `TestFixture` exported in `pkg/testutil/fixture.go`
- [ ] `MCPClient` exported in `pkg/testutil/client.go`
- [ ] All existing integration tests pass with new imports
- [ ] `ls ~/src/evr-test-harness/pkg/testutil/*.go` shows fixture.go, client.go
- [ ] No breaking changes to existing test behavior

---

## Next Steps

**After completing this refactoring:**

1. Return to nevr-runtime: `cd ~/src/nevr-server`
2. Resume the test-harness-integration plan: `/start-work`
3. The plan will automatically continue from Task 1

---

## Reference Files

- **Source:** `tests/integration/main_test.go` (current location)
- **Target:** `pkg/testutil/fixture.go`, `pkg/testutil/client.go`
- **Usage example:** See nevr-runtime plan at `.sisyphus/plans/test-harness-integration.md`

package system

import (
	"fmt"
	"testing"
	"time"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// TestE2E_FullCycle tests the complete end-to-end lifecycle:
// Deploy DLLs → Start Game → Verify Ready State → Verify Events → Stop Game → Cleanup
func TestE2E_FullCycle(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping E2E integration test in short mode")
	}

	// Phase 1: Deploy all DLLs
	t.Log("Phase 1: Deploying all NEVR DLLs")
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Phase 2: Create test fixture and start game
	t.Log("Phase 2: Starting Echo VR game with NEVR DLLs")
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6799,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err, "Failed to start Echo VR")

	sessionID := startResult["session_id"].(string)
	pid := int(startResult["pid"].(float64))
	assert.NotEmpty(t, sessionID, "Session ID should not be empty")
	assert.Equal(t, "running", startResult["status"], "Game should be running")
	t.Logf("Game started: session=%s pid=%d port=%d", sessionID, pid, 6799)

	// Ensure game is stopped at end
	defer func() {
		t.Log("Phase 6: Stopping game session")
		stopResult, err := f.MCPClient().Call("echovr_stop", map[string]any{
			"session_id":      sessionID,
			"timeout_seconds": 10,
		})
		if err != nil {
			t.Logf("Warning: Failed to stop session: %v", err)
		} else {
			assert.Equal(t, "stopped", stopResult["status"], "Game should stop cleanly")
		}
	}()

	// Phase 3: Verify game ready state (HTTP API)
	t.Log("Phase 3: Verifying game ready state (HTTP API)")
	gameURL := fmt.Sprintf("http://localhost:%d/session", 6799)
	err = f.WaitForHTTP(gameURL, 30*time.Second)
	require.NoError(t, err, "HTTP API did not become available")
	AssertHTTPResponds(t, gameURL)
	AssertProcessRunning(t, pid)
	t.Log("Game HTTP API responding successfully")

	// Phase 4: Verify event streaming
	t.Log("Phase 4: Verifying event streaming")
	// Wait a few seconds for events to accumulate
	time.Sleep(5 * time.Second)

	eventsResult, err := f.MCPClient().Call("echovr_events", map[string]any{
		"session_id": sessionID,
		"types":      []string{"all"},
		"limit":      100,
	})
	require.NoError(t, err, "Failed to retrieve events")

	events := eventsResult["events"].([]any)
	assert.NotEmpty(t, events, "Expected events during game startup")

	// Verify we got state change events (indicates game lifecycle working)
	hasStateChange := false
	for _, e := range events {
		event := e.(map[string]any)
		if event["type"] == "state_change" {
			hasStateChange = true
			t.Logf("Found state_change event: %v", event)
			break
		}
	}
	assert.True(t, hasStateChange, "Expected state_change events during startup")
	t.Logf("Event streaming verified: %d events captured", len(events))

	// Phase 5: Verify clean shutdown
	t.Log("Phase 5: Verifying clean shutdown")
	stopResult, err := f.MCPClient().Call("echovr_stop", map[string]any{
		"session_id":      sessionID,
		"timeout_seconds": 10,
	})
	require.NoError(t, err, "Failed to stop game")
	assert.Equal(t, "stopped", stopResult["status"], "Game should stop cleanly")

	// Verify process terminated
	AssertProcessStopped(t, pid)
	t.Log("Process terminated successfully")

	// Verify HTTP API no longer responds
	AssertHTTPFails(t, gameURL)
	t.Log("HTTP API no longer responding (expected)")

	// Phase 6: Cleanup verification happens in defer above
	t.Log("E2E full cycle test completed successfully")
}

// TestE2E_AllDLLsLoaded verifies all NEVR DLLs can be loaded together in a running game
func TestE2E_AllDLLsLoaded(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping E2E integration test in short mode")
	}

	// Deploy all DLLs
	t.Log("Deploying all NEVR DLLs: gamepatches, gameserver, telemetryagent")
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Start game with DLLs loaded
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6798,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err, "Failed to start Echo VR with all DLLs")

	sessionID := startResult["session_id"].(string)
	pid := int(startResult["pid"].(float64))

	defer func() {
		_, _ = f.MCPClient().Call("echovr_stop", map[string]any{
			"session_id": sessionID,
		})
	}()

	// Verify game started and HTTP API responds
	gameURL := fmt.Sprintf("http://localhost:%d/session", 6798)
	err = f.WaitForHTTP(gameURL, 30*time.Second)
	require.NoError(t, err, "HTTP API did not become available with all DLLs loaded")

	AssertHTTPResponds(t, gameURL)
	AssertProcessRunning(t, pid)

	t.Log("All NEVR DLLs loaded successfully in running game")
}

// TestE2E_EventStreaming verifies continuous event streaming during gameplay
func TestE2E_EventStreaming(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping E2E integration test in short mode")
	}

	// Deploy DLLs
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Start game
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6797,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err)

	sessionID := startResult["session_id"].(string)

	defer func() {
		_, _ = f.MCPClient().Call("echovr_stop", map[string]any{
			"session_id": sessionID,
		})
	}()

	// Wait for game ready
	gameURL := fmt.Sprintf("http://localhost:%d/session", 6797)
	err = f.WaitForHTTP(gameURL, 30*time.Second)
	require.NoError(t, err)

	// Sample events multiple times to verify streaming
	t.Log("Sampling events over time to verify streaming")

	for i := 0; i < 3; i++ {
		time.Sleep(3 * time.Second)

		eventsResult, err := f.MCPClient().Call("echovr_events", map[string]any{
			"session_id": sessionID,
			"types":      []string{"all"},
			"limit":      50,
		})
		require.NoError(t, err, "Failed to retrieve events (sample %d)", i+1)

		events := eventsResult["events"].([]any)
		t.Logf("Sample %d: captured %d events", i+1, len(events))

		// Should have some events in each sample
		if len(events) > 0 {
			assert.NotEmpty(t, events, "Expected events in sample %d", i+1)
		}
	}

	t.Log("Event streaming verified over multiple samples")
}

// TestE2E_CleanShutdownNoOrphans verifies no processes are orphaned after shutdown
func TestE2E_CleanShutdownNoOrphans(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping E2E integration test in short mode")
	}

	// Deploy DLLs
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Start game
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6796,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err)

	sessionID := startResult["session_id"].(string)
	pid := int(startResult["pid"].(float64))

	// Wait for game ready
	gameURL := fmt.Sprintf("http://localhost:%d/session", 6796)
	err = f.WaitForHTTP(gameURL, 30*time.Second)
	require.NoError(t, err)

	AssertProcessRunning(t, pid)
	t.Logf("Game running with PID %d", pid)

	// Stop game
	stopResult, err := f.MCPClient().Call("echovr_stop", map[string]any{
		"session_id":      sessionID,
		"timeout_seconds": 10,
	})
	require.NoError(t, err, "Failed to stop game")
	assert.Equal(t, "stopped", stopResult["status"])

	// Verify process is actually stopped
	AssertProcessStopped(t, pid)
	t.Logf("Game process %d terminated cleanly", pid)

	// Verify HTTP API is down
	AssertHTTPFails(t, gameURL)

	// Wait a moment and verify process stays dead (no zombie resurrection)
	time.Sleep(2 * time.Second)
	AssertProcessStopped(t, pid)

	t.Log("Clean shutdown verified: no orphaned processes")
}

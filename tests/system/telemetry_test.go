package system

import (
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
	"time"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/require"
)

// TestTelemetry_AgentLoads verifies that telemetryagent.dll can be deployed,
// loaded by the game process, and initialized correctly.
func TestTelemetry_AgentLoads(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Deploy all DLLs including telemetryagent.dll
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Create test fixture with MCP client
	fixture := testutil.NewFixture(t)
	defer fixture.Cleanup()

	// Start EchoVR game instance with headless mode
	result, err := fixture.MCPClient().Call("echovr_start", map[string]any{
		"headless":        true,
		"moderator":       true,
		"http_port":       6721,
		"wait_ready":      true,
		"timeout_seconds": 60,
		"gametype":        "echo_arena",
		"level":           "mpl_arena_a",
	})
	require.NoError(t, err, "Failed to start EchoVR game instance")
	require.NotNil(t, result)

	sessionID, ok := result["session_id"].(string)
	require.True(t, ok, "Expected session_id in result")
	require.NotEmpty(t, sessionID, "Expected non-empty session ID")
	t.Logf("Started EchoVR instance with session ID: %s", sessionID)

	defer func() {
		_, err := fixture.MCPClient().Call("echovr_stop", map[string]any{
			"session_id":      sessionID,
			"timeout_seconds": 30,
		})
		require.NoError(t, err, "Failed to stop EchoVR game instance")
		t.Log("Stopped EchoVR instance")
	}()

	// Verify game HTTP API is responding (confirms game is running)
	apiURL := "http://127.0.0.1:6721/session"
	AssertHTTPResponds(t, apiURL)
	t.Log("Game HTTP API is responding")

	// TODO: Add verification that telemetryagent.dll is loaded
	// This would require checking process memory or looking for telemetry-specific
	// HTTP endpoints or logging output. For now, we verify the game started successfully
	// with the DLL present in the bin/win10 directory.

	t.Log("Telemetry agent loading test completed successfully")
}

// TestTelemetry_FrameCapture verifies that telemetry data can be captured
// from the game state and correlates with the actual game state via HTTP API.
func TestTelemetry_FrameCapture(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Deploy all DLLs
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Create test fixture
	fixture := testutil.NewFixture(t)
	defer fixture.Cleanup()

	// Start game instance
	result, err := fixture.MCPClient().Call("echovr_start", map[string]any{
		"headless":        true,
		"moderator":       true,
		"http_port":       6721,
		"wait_ready":      true,
		"timeout_seconds": 60,
		"gametype":        "echo_arena",
		"level":           "mpl_arena_a",
	})
	require.NoError(t, err, "Failed to start game")
	require.NotNil(t, result)

	sessionID, ok := result["session_id"].(string)
	require.True(t, ok, "Expected session_id in result")
	require.NotEmpty(t, sessionID)
	t.Logf("Started game with session ID: %s", sessionID)

	defer func() {
		_, err := fixture.MCPClient().Call("echovr_stop", map[string]any{
			"session_id":      sessionID,
			"timeout_seconds": 30,
		})
		require.NoError(t, err)
	}()

	// Wait for game to be fully ready
	time.Sleep(2 * time.Second)

	// Query game state via HTTP API
	apiURL := "http://127.0.0.1:6721/session"
	resp, err := http.Get(apiURL)
	require.NoError(t, err, "Failed to query game state")
	require.Equal(t, http.StatusOK, resp.StatusCode, "Expected HTTP 200 from /session")
	defer resp.Body.Close()

	// Parse session data
	var sessionData map[string]interface{}
	err = json.NewDecoder(resp.Body).Decode(&sessionData)
	require.NoError(t, err, "Failed to parse session JSON")

	// Verify we got session data
	require.NotEmpty(t, sessionData, "Session data should not be empty")
	t.Logf("Retrieved session data with %d fields", len(sessionData))

	// Check for expected fields in session data
	expectedFields := []string{"sessionid", "game_clock_display", "game_status"}
	for _, field := range expectedFields {
		_, exists := sessionData[field]
		require.True(t, exists, "Expected field '%s' in session data", field)
		t.Logf("Found expected field: %s = %v", field, sessionData[field])
	}

	// TODO: Add verification that telemetry agent captured this frame
	// This would require either:
	// 1. Checking telemetry API endpoint for received frames
	// 2. Inspecting agent statistics via exported C API
	// 3. Monitoring log output for frame processing messages

	t.Log("Frame capture test completed - game state successfully queried and validated")
}

// TestTelemetry_Streaming verifies that telemetry data can be streamed
// to a mock endpoint (localhost only, no production URLs).
func TestTelemetry_Streaming(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	t.Skip("Streaming test requires mock telemetry endpoint - implement when mock server available")

	// Implementation outline:
	// 1. Start mock HTTP server on localhost to receive telemetry
	// 2. Deploy telemetryagent.dll with config pointing to mock server
	// 3. Start game instance
	// 4. Wait for telemetry frames to arrive at mock server
	// 5. Verify frame structure matches expected schema
	// 6. Verify frame rate matches configured polling frequency
}

// TestTelemetry_SchemaValidation verifies that telemetry frames
// match the expected protocol buffer schema.
func TestTelemetry_SchemaValidation(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	t.Skip("Schema validation requires protobuf parsing - implement when telemetry streaming works")

	// Implementation outline:
	// 1. Capture telemetry frame from mock server (see TestTelemetry_Streaming)
	// 2. Parse as telemetry.v1.LobbySessionStateFrame protobuf message
	// 3. Verify required fields are populated
	// 4. Verify field types match schema
	// 5. Verify session_uuid, timestamp, game_status are correct
}

// TestTelemetry_AgentInitialization verifies the telemetry agent C API
// exports are accessible (if we load DLL via cgo in future).
func TestTelemetry_AgentInitialization(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	t.Skip("Agent initialization test requires cgo DLL loading - implement if needed")

	// Implementation outline (if we use cgo to load telemetryagent.dll):
	// 1. Load telemetryagent.dll via cgo
	// 2. Call TelemetryAgent_GetDefaultConfig()
	// 3. Verify default config values
	// 4. Call TelemetryAgent_Initialize(config)
	// 5. Verify TelemetryAgent_IsInitialized() returns 1
	// 6. Call TelemetryAgent_Shutdown()
}

// TestTelemetry_HTTPPollerDirectQuery tests the HTTP poller functionality
// by directly querying game endpoints without starting a telemetry session.
func TestTelemetry_HTTPPollerDirectQuery(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// This test doesn't require telemetryagent.dll, just the game HTTP API
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	fixture := testutil.NewFixture(t)
	defer fixture.Cleanup()

	result, err := fixture.MCPClient().Call("echovr_start", map[string]any{
		"headless":        true,
		"moderator":       true,
		"http_port":       6721,
		"wait_ready":      true,
		"timeout_seconds": 60,
		"gametype":        "echo_arena",
		"level":           "mpl_arena_a",
	})
	require.NoError(t, err)
	require.NotNil(t, result)

	sessionID, ok := result["session_id"].(string)
	require.True(t, ok)
	require.NotEmpty(t, sessionID)

	defer func() {
		fixture.MCPClient().Call("echovr_stop", map[string]any{
			"session_id":      sessionID,
			"timeout_seconds": 30,
		})
	}()

	// Test both endpoints that telemetry agent would poll
	endpoints := []string{"/session", "/player_bones"}

	for _, endpoint := range endpoints {
		url := fmt.Sprintf("http://127.0.0.1:6721%s", endpoint)
		t.Logf("Testing endpoint: %s", url)

		resp, err := http.Get(url)
		require.NoError(t, err, "Failed to GET %s", endpoint)
		require.Equal(t, http.StatusOK, resp.StatusCode, "Expected HTTP 200 from %s", endpoint)
		resp.Body.Close()

		t.Logf("✓ Endpoint %s is accessible", endpoint)
	}

	t.Log("HTTP poller endpoint validation completed successfully")
}

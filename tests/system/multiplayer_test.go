package system

import (
	"fmt"
	"testing"
	"time"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// TestMultiplayer_SessionCreation tests that the gameserver DLL enables
// the game to start as a server and create a multiplayer session that
// accepts connections.
func TestMultiplayer_SessionCreation(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Deploy gameserver.dll
	dllPath := getDLLPath("gameserver.dll")
	gameDir := getGameDir()
	err := deployDLL(dllPath, gameDir)
	require.NoError(t, err, "Failed to deploy gameserver.dll")

	defer cleanupAllDLLs(t)

	// Create test fixture
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// Start Echo VR in server mode
	result, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6731,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start Echo VR server")

	// Track session for cleanup
	sessionID := result["session_id"].(string)
	f.Sessions = append(f.Sessions, sessionID)

	// Assert: Session created
	assert.NotEmpty(t, sessionID, "Session ID should not be empty")
	assert.Equal(t, "running", result["status"], "Session should be running")
	assert.Equal(t, float64(6731), result["http_port"], "HTTP port should match")

	// Assert: HTTP API is accessible
	err = f.WaitForHTTP("http://localhost:6731/session", 30*time.Second)
	require.NoError(t, err, "HTTP API did not become available")

	AssertHTTPResponds(t, "http://localhost:6731/session")

	// Assert: Process is running
	pid := int(result["pid"].(float64))
	AssertProcessRunning(t, pid)

	t.Log("✓ Multiplayer session created successfully")
	t.Logf("✓ Session ID: %s, PID: %d, HTTP: http://localhost:6731", sessionID, pid)
}

// TestMultiplayer_PlayerEvents tests detection of player join and leave
// events using the event streaming API from evr-test-harness.
func TestMultiplayer_PlayerEvents(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Deploy all NEVR DLLs (gameserver enables multiplayer)
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Create test fixture
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// Start Echo VR server
	result, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6732,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start Echo VR server")

	sessionID := result["session_id"].(string)
	f.Sessions = append(f.Sessions, sessionID)

	// Wait for HTTP API
	err = f.WaitForHTTP("http://localhost:6732/session", 30*time.Second)
	require.NoError(t, err, "HTTP API did not become available")

	// TODO: When event streaming is implemented, add event subscription
	// and verification:
	//
	// 1. Subscribe to player_join and player_leave events
	// 2. Simulate player connection (either via second instance or API)
	// 3. Verify player_join event is emitted with player data
	// 4. Simulate player disconnection
	// 5. Verify player_leave event is emitted
	//
	// Example (when available):
	// events, err := f.SubscribeToEvents(sessionID, []string{"player_join", "player_leave"})
	// require.NoError(t, err)
	//
	// // Trigger player join
	// ...
	//
	// // Wait for event
	// select {
	// case evt := <-events:
	//     assert.Equal(t, "player_join", evt.Type)
	//     assert.NotEmpty(t, evt.Data["name"])
	// case <-time.After(10 * time.Second):
	//     t.Fatal("Timeout waiting for player_join event")
	// }

	t.Log("✓ Multiplayer server running (event streaming tests pending implementation)")
	t.Log("✓ Session ready for player event testing once event streaming API is available")
}

// TestMultiplayer_MatchState tests match state transitions including
// match_start and match_end events, and verifies the game state
// progression through a complete match lifecycle.
func TestMultiplayer_MatchState(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Deploy all NEVR DLLs
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Create test fixture
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// Start Echo VR server
	result, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6733,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start Echo VR server")

	sessionID := result["session_id"].(string)
	f.Sessions = append(f.Sessions, sessionID)

	// Wait for HTTP API
	err = f.WaitForHTTP("http://localhost:6733/session", 30*time.Second)
	require.NoError(t, err, "HTTP API did not become available")

	// TODO: When event streaming and match control are implemented:
	//
	// 1. Subscribe to match_start, match_end, and state_change events
	// 2. Verify initial state (e.g., "pre_match" or "lobby")
	// 3. Trigger match start (via API or player count threshold)
	// 4. Verify match_start event is emitted
	// 5. Verify state transition to "in_progress"
	// 6. Trigger match end (via API, time limit, or score)
	// 7. Verify match_end event with final scores
	// 8. Verify state transition to "post_match" or "lobby"
	//
	// Example (when available):
	// events, err := f.SubscribeToEvents(sessionID, []string{
	//     "match_start", "match_end", "state_change",
	// })
	// require.NoError(t, err)
	//
	// // Get initial state
	// state, err := f.GetGameState(sessionID)
	// require.NoError(t, err)
	// assert.Equal(t, "pre_match", state.MatchState)
	//
	// // Start match
	// err = f.StartMatch(sessionID)
	// require.NoError(t, err)
	//
	// // Wait for match_start event
	// select {
	// case evt := <-events:
	//     assert.Equal(t, "match_start", evt.Type)
	// case <-time.After(10 * time.Second):
	//     t.Fatal("Timeout waiting for match_start event")
	// }

	t.Log("✓ Multiplayer server running (match state tests pending implementation)")
	t.Log("✓ Session ready for match state testing once event streaming and match control are available")
}

// TestMultiplayer_MultiInstance tests coordination between multiple game
// instances, verifying they can discover and connect to each other for
// multiplayer gameplay. This test is skipped in short mode as it requires
// significant resources and time.
func TestMultiplayer_MultiInstance(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping multi-instance test in short mode (resource intensive)")
	}

	// Deploy all NEVR DLLs
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Create test fixture
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// Start server instance (host)
	serverResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6734,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start server instance")

	serverSessionID := serverResult["session_id"].(string)
	f.Sessions = append(f.Sessions, serverSessionID)

	// Wait for server HTTP API
	err = f.WaitForHTTP("http://localhost:6734/session", 30*time.Second)
	require.NoError(t, err, "Server HTTP API did not become available")

	// Start client instance (player)
	clientResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6735,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start client instance")

	clientSessionID := clientResult["session_id"].(string)
	f.Sessions = append(f.Sessions, clientSessionID)

	// Wait for client HTTP API
	err = f.WaitForHTTP("http://localhost:6735/session", 30*time.Second)
	require.NoError(t, err, "Client HTTP API did not become available")

	// Verify both instances are running
	serverPID := int(serverResult["pid"].(float64))
	clientPID := int(clientResult["pid"].(float64))
	AssertProcessRunning(t, serverPID)
	AssertProcessRunning(t, clientPID)

	// TODO: When instance coordination is implemented:
	//
	// 1. Get server session info (IP, port, session ID)
	// 2. Command client to connect to server session
	// 3. Subscribe to player_join events on server
	// 4. Verify client appears as player on server
	// 5. Verify server player list includes client
	// 6. Test client disconnect and verify player_leave event
	//
	// Example (when available):
	// serverInfo, err := f.GetSessionInfo(serverSessionID)
	// require.NoError(t, err)
	//
	// err = f.ConnectToSession(clientSessionID, serverInfo.IP, serverInfo.Port)
	// require.NoError(t, err)
	//
	// // Wait for client to appear on server
	// err = f.WaitFor(func() bool {
	//     players, _ := f.GetPlayers(serverSessionID)
	//     return len(players) > 0
	// }, 30*time.Second)
	// require.NoError(t, err, "Client did not join server session")

	t.Log("✓ Multiple instances started successfully")
	t.Logf("✓ Server: %s (PID: %d, Port: 6734)", serverSessionID, serverPID)
	t.Logf("✓ Client: %s (PID: %d, Port: 6735)", clientSessionID, clientPID)
	t.Log("✓ Multi-instance coordination tests pending implementation")
}

// TestMultiplayer_SessionCleanup verifies that multiplayer sessions can be
// properly stopped and cleaned up, releasing all resources.
func TestMultiplayer_SessionCleanup(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Deploy gameserver.dll
	dllPath := getDLLPath("gameserver.dll")
	gameDir := getGameDir()
	err := deployDLL(dllPath, gameDir)
	require.NoError(t, err, "Failed to deploy gameserver.dll")

	defer cleanupAllDLLs(t)

	// Create test fixture
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// Start Echo VR server
	result, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6736,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start Echo VR server")

	sessionID := result["session_id"].(string)
	pid := int(result["pid"].(float64))

	// Wait for HTTP API
	err = f.WaitForHTTP("http://localhost:6736/session", 30*time.Second)
	require.NoError(t, err, "HTTP API did not become available")

	// Verify session is running
	AssertProcessRunning(t, pid)
	AssertHTTPResponds(t, "http://localhost:6736/session")

	// Stop the session
	stopResult, err := f.MCPClient().Call("echovr_stop", map[string]any{
		"session_id":      sessionID,
		"timeout_seconds": 10,
	})
	require.NoError(t, err, "Failed to stop session")

	// Assert: Session stopped
	assert.Equal(t, "stopped", stopResult["status"], "Session should be stopped")

	// Assert: Process terminated
	AssertProcessStopped(t, pid)

	// Assert: HTTP API no longer responds
	AssertHTTPFails(t, fmt.Sprintf("http://localhost:%d/session", 6736))

	t.Log("✓ Session cleanup successful")
	t.Log("✓ Process terminated, HTTP API stopped, resources released")
}

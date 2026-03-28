package plugins

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"testing"
	"time"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// IMPORTANT: These integration tests must NOT use t.Parallel().
// All tests share a single session_api instance on port 6721, and only one
// game process can run at a time. Tests within a Go package run sequentially
// by default — do not add t.Parallel() to any test in this file.

type sessionResponse struct {
	GameStatus   string  `json:"game_status"`
	GameModeHash string  `json:"game_mode_hash"`
	GameClock    float64 `json:"game_clock"`
	BluePoints   int     `json:"blue_points"`
	OrangePoints int     `json:"orange_points"`
}

// sessionApiPort is the port session_api listens on (default from session_api.h).
// This is SEPARATE from the game's built-in HTTP API port (http_port in echovr_start).
// The game's API does NOT include game_mode_hash; session_api's does.
const sessionApiPort = 6721

// fetchSessionFromPlugin queries the session_api plugin's HTTP server (NOT the game's).
func fetchSessionFromPlugin(t *testing.T) sessionResponse {
	t.Helper()
	url := fmt.Sprintf("http://localhost:%d/session", sessionApiPort)
	resp, err := http.Get(url)
	require.NoError(t, err, "GET /session from session_api (port %d) failed", sessionApiPort)
	defer resp.Body.Close()
	require.Equal(t, 200, resp.StatusCode)

	body, err := io.ReadAll(resp.Body)
	require.NoError(t, err)

	var session sessionResponse
	require.NoError(t, json.Unmarshal(body, &session),
		"Failed to parse session JSON: %s", string(body))
	return session
}

func deployPluginTestStack(t *testing.T, plugins ...string) {
	t.Helper()
	deployAllCoreDLLs(t)
	deployPlugins(t, plugins...)
}

// waitForSessionApi waits for the session_api plugin's HTTP server to respond.
func waitForSessionApi(t *testing.T, f *testutil.TestFixture) {
	t.Helper()
	url := fmt.Sprintf("http://localhost:%d/session", sessionApiPort)
	err := f.WaitForHTTP(url, 30*time.Second)
	require.NoError(t, err,
		"session_api HTTP API (port %d) did not become available", sessionApiPort)
}

func TestSuperHyperTurbo_ArenaGetsCombatWeapons(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployPluginTestStack(t, "session_api", "super_hyper_turbo")
	defer cleanupAll(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// http_port is the GAME's built-in HTTP API — used for readiness check only.
	// session_api runs its own server on port 6721 (sessionApiPort).
	const gameHttpPort = 6780
	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       gameHttpPort,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err, "Failed to start Echo VR")

	sessionID := startResult["session_id"].(string)
	pid := int(startResult["pid"].(float64))
	t.Logf("Game started: session=%s pid=%d", sessionID, pid)

	defer func() {
		_, err := f.MCPClient().Call("echovr_stop", map[string]any{
			"session_id":      sessionID,
			"timeout_seconds": 10,
		})
		if err != nil {
			t.Logf("Warning: stop failed: %v", err)
		}
	}()

	// Wait for session_api plugin's HTTP server (port 6721), NOT the game's
	waitForSessionApi(t, f)

	session := fetchSessionFromPlugin(t)
	t.Logf("Session: status=%s game_mode_hash=%s", session.GameStatus, session.GameModeHash)

	// Assert: game_mode_hash should be the COMBAT hash because super-hyper-turbo
	// swaps arena hashes to combat before CreateSession runs.
	assert.Equal(t, "0x47c693e5ebaddea6", session.GameModeHash,
		"Arena mode should be swapped to HASH_COMBAT_PRACTICE_AI")

	AssertProcessRunning(t, pid)
}

func TestSuperHyperTurbo_PluginInitSucceeds(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployPluginTestStack(t, "session_api", "super_hyper_turbo")
	defer cleanupAll(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	const gameHttpPort = 6781
	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       gameHttpPort,
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

	// Wait for session_api's HTTP server
	waitForSessionApi(t, f)

	session := fetchSessionFromPlugin(t)

	// game_mode_hash present → session_api loaded
	assert.NotEmpty(t, session.GameModeHash, "game_mode_hash should be present")
	// game_mode_hash is combat → super_hyper_turbo loaded and hooked
	assert.Equal(t, "0x47c693e5ebaddea6", session.GameModeHash,
		"game_mode_hash should be combat hash (super_hyper_turbo loaded)")

	t.Log("Both session_api and super_hyper_turbo initialized successfully")
}

func TestSuperHyperTurbo_NonArenaModePassthrough(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployPluginTestStack(t, "session_api", "super_hyper_turbo")
	defer cleanupAll(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	const gameHttpPort = 6782
	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       gameHttpPort,
		"gametype":        "echo_combat_private",
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

	waitForSessionApi(t, f)

	session := fetchSessionFromPlugin(t)
	t.Logf("Combat mode session: game_mode_hash=%s", session.GameModeHash)

	assert.NotEmpty(t, session.GameModeHash)

	// These 6 hashes appear ONLY in IsArenaMode, not in any combat classifier
	arenaOnlyHashes := map[string]bool{
		"0xcb60a4de7e1caf73": true,
		"0x09990965f4db8c03": true,
		"0xd53a9c73b8b2c760": true,
		"0x4dc1ddbd89b28531": true,
		"0x339c5dc66c03b81f": true,
		"0xd894afea8246bc6b": true,
	}

	assert.False(t, arenaOnlyHashes[session.GameModeHash],
		"Combat mode should not have arena-only hash, got %s", session.GameModeHash)
}

func TestSuperHyperTurbo_CleanShutdown(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployPluginTestStack(t, "session_api", "super_hyper_turbo")
	defer cleanupAll(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	const gameHttpPort = 6783
	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       gameHttpPort,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err)

	sessionID := startResult["session_id"].(string)
	pid := int(startResult["pid"].(float64))

	waitForSessionApi(t, f)
	AssertProcessRunning(t, pid)

	stopResult, err := f.MCPClient().Call("echovr_stop", map[string]any{
		"session_id":      sessionID,
		"timeout_seconds": 10,
	})
	require.NoError(t, err)
	assert.Equal(t, "stopped", stopResult["status"])

	AssertProcessStopped(t, pid)

	// Verify session_api's HTTP server is down too
	sessionApiURL := fmt.Sprintf("http://localhost:%d/session", sessionApiPort)
	AssertHTTPFails(t, sessionApiURL)

	t.Log("Clean shutdown verified (MH_RemoveHook working)")
}

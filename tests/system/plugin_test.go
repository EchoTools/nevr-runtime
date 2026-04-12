package system

import (
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/require"
)

// TestPlugin_ServerTimingSkipsClientMode verifies the server_timing plugin does NOT
// apply its patches when the game runs in client mode (without -server flag).
//
// The server_timing plugin hooks CPrecisionSleep::Wait, patches BusyWait to RET,
// and optionally hooks SwitchToThread -- all optimizations meant for headless
// dedicated servers that would degrade or crash a game client. The plugin guards
// on NEVR_HOST_IS_SERVER and must skip all patches when running as a client.
//
// This test loads the plugin in client mode and verifies the process remains
// stable after startup, proving the guard works.
func TestPlugin_ServerTimingSkipsClientMode(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	gameDir := getGameDir()

	// Deploy gamepatches.dll (required for CLI flag support and plugin loading)
	gamepatchesDLL := getDLLPath("gamepatches.dll")
	if _, err := os.Stat(gamepatchesDLL); os.IsNotExist(err) {
		t.Skipf("gamepatches.dll not found at %s (build required)", gamepatchesDLL)
	}
	err := deployDLL(gamepatchesDLL, gameDir)
	require.NoError(t, err, "Failed to deploy gamepatches.dll")

	// Deploy server_timing.dll plugin to plugins/ subdirectory
	serverTimingDLL := getPluginPath("server_timing.dll")
	if _, err := os.Stat(serverTimingDLL); os.IsNotExist(err) {
		t.Skipf("server_timing.dll not found at %s (build required)", serverTimingDLL)
	}
	err = deployPlugin(serverTimingDLL, gameDir)
	require.NoError(t, err, "Failed to deploy server_timing.dll plugin")

	defer func() {
		cleanupAllDLLs(t)
		err := cleanupPlugins(gameDir)
		require.NoError(t, err, "Failed to cleanup plugins")
	}()

	// Start the game in CLIENT mode (server_mode: false)
	f := testutil.NewFixture(t)
	defer f.Cleanup()

	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6750,
		"server_mode":     false,
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start Echo VR in client mode")

	sessionID := startResult["session_id"].(string)
	pid := int(startResult["pid"].(float64))
	t.Logf("Client started: session=%s pid=%d port=%d", sessionID, pid, 6750)

	defer func() {
		_, _ = f.MCPClient().Call("echovr_stop", map[string]any{
			"session_id":      sessionID,
			"timeout_seconds": 10,
		})
	}()

	// Verify process is running and HTTP API responds
	AssertProcessRunning(t, pid)
	gameURL := fmt.Sprintf("http://localhost:%d/session", 6750)
	AssertHTTPResponds(t, gameURL)

	// Wait 5 seconds to let any incorrectly-applied timing patches destabilize
	// the process. If the server_timing plugin fired its hooks on a client,
	// the BusyWait RET patch and Sleep hook would cause hangs or crashes.
	t.Log("Waiting 5 seconds to verify process stability with server_timing loaded in client mode")
	time.Sleep(5 * time.Second)

	// Process must still be running -- if patches fired, the client would
	// have crashed or hung by now
	AssertProcessRunning(t, pid)
	AssertHTTPResponds(t, gameURL)
	t.Log("Client process stable with server_timing plugin loaded (patches correctly skipped)")
}

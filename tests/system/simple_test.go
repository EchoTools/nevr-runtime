package system

import (
	"testing"
	"time"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/require"
)

func TestBasicEchoVRStart(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	fixture := testutil.NewFixture(t)
	defer fixture.Cleanup()

	t.Logf("Starting Echo VR server...")
	result, err := fixture.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6721,
		"server_mode":     true,
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start Echo VR")

	sessionID := result["session_id"].(string)
	pid := int(result["pid"].(float64))
	httpPort := int(result["http_port"].(float64))

	t.Logf("✓ Server started!")
	t.Logf("  - Session ID: %s", sessionID)
	t.Logf("  - PID: %d", pid)
	t.Logf("  - HTTP Port: %d", httpPort)

	fixture.Sessions = append(fixture.Sessions, sessionID)

	t.Logf("Waiting 30 seconds to observe server behavior...")
	time.Sleep(30 * time.Second)

	t.Logf("✅ Test complete - server ran for 30 seconds")
}

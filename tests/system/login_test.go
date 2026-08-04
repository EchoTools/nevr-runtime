package system

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// These tests exercise a real signed-in client.  They are opt-in because they
// use the operator's local Echo installation and its cached account session;
// unit and ground-truth tests remain the default CI path.
const envLiveTests = "NEVR_LIVE_TESTS"

var liveLoginRun struct {
	once   sync.Once
	output string
	err    error
}

func requireLiveClient(t *testing.T) string {
	t.Helper()
	if testing.Short() || os.Getenv(envLiveTests) != "1" {
		t.Skip("set NEVR_LIVE_TESTS=1 to run the local Echo VR login smoke test")
	}

	liveLoginRun.once.Do(func() {
		gameDir := getGameDir()
		binDir := filepath.Join(gameDir, "bin", "win10")
		prefix := os.Getenv("WINEPREFIX")
		if prefix == "" {
			prefix = filepath.Join(gameDir, ".wineprefix")
		}

		// Private arena reaches the required login/session-reuse evidence well
		// before this bound; it deliberately avoids waiting on matchmaking.
		ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
		defer cancel()
		cmd := exec.CommandContext(ctx, "wine", "./echovr.exe", "-windowed", "-noaudio", "-mp",
			"-gametype", "echo_arena_private")
		cmd.Dir = binDir
		cmd.Env = append(os.Environ(), "DISPLAY=:101", "WINEPREFIX="+prefix)
		output, err := cmd.CombinedOutput()
		liveLoginRun.output = string(output)
		liveLoginRun.err = err
		if ctx.Err() == context.DeadlineExceeded {
			// A successful game remains running; the assertions below inspect the
			// boot/login markers accumulated before the bounded test terminates it.
			liveLoginRun.err = nil
		}
	})

	if liveLoginRun.err != nil {
		t.Fatalf("echovr.exe could not be run: %v\n%s", liveLoginRun.err, liveLoginRun.output)
	}
	return liveLoginRun.output
}

func TestLoginBuildIdentityInLog(t *testing.T) {
	output := requireLiveClient(t)
	if !strings.Contains(output, "[NEVR.BOOT] runtime bootstrap complete") {
		t.Fatalf("runtime bootstrap marker missing\n%s", output)
	}
}

func TestLoginPlatformCodeAndInjectionInLog(t *testing.T) {
	output := requireLiveClient(t)
	if !strings.Contains(output, "[NEVR.WS] login injected xpid=OVR-ORG-") {
		t.Fatalf("OVR-ORG platform login injection marker missing\n%s", output)
	}
}

func TestLoginWebSocketBridgeReachesLoggedIn(t *testing.T) {
	output := requireLiveClient(t)
	for _, marker := range []string{
		"[NEVR.WS] LOGIN SUCCESS",
		"NetGame switching state (from logging in, to logged in)",
		"sharing login session (no LoginRequest)",
	} {
		if !strings.Contains(output, marker) {
			t.Fatalf("missing %q\n%s", marker, output)
		}
	}
}

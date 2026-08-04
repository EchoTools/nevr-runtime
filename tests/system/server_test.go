package system

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

// Server tests are deliberately gated separately from the client smoke test.
// A dedicated server has different startup and shutdown paths, and must only be
// started by an operator who explicitly opted in to that local exercise.
const envLiveServerTests = "NEVR_LIVE_SERVER_TESTS"

const (
	serverStartupTimeout  = 60 * time.Second
	serverShutdownTimeout = 5 * time.Second
	serverSmokeScratchDir = "/var/tmp/work-nevr-runtime"
)

// lockedBuffer permits the readiness poller and exec.Cmd's output goroutine to
// access process output concurrently without a data race.
type lockedBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (b *lockedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.Write(p)
}

func (b *lockedBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.String()
}

var liveServerRun struct {
	once   sync.Once
	output string
	err    error
}

func requireLiveServer(t *testing.T) string {
	t.Helper()
	if testing.Short() || os.Getenv(envLiveServerTests) != "1" {
		t.Skip("set NEVR_LIVE_SERVER_TESTS=1 to run the local Echo VR server smoke test")
	}

	liveServerRun.once.Do(func() {
		liveServerRun.output, liveServerRun.err = runLiveServerSmoke()
	})
	if liveServerRun.err != nil {
		t.Fatalf("dedicated server smoke failed: %v\n%s", liveServerRun.err, liveServerRun.output)
	}
	return liveServerRun.output
}

func runLiveServerSmoke() (string, error) {
	gameDir := getGameDir()
	binDir := filepath.Join(gameDir, "bin", "win10")
	prefix := os.Getenv("WINEPREFIX")
	if prefix == "" {
		prefix = filepath.Join(gameDir, ".wineprefix")
	}

	// Do not inherit an operator's service endpoints.  Both config layers route
	// every service URL used by this smoke to loopback, preventing a connection
	// to or registration with production.
	if err := os.MkdirAll(serverSmokeScratchDir, 0700); err != nil {
		return "", fmt.Errorf("create server smoke scratch directory: %w", err)
	}
	configDir, err := os.MkdirTemp(serverSmokeScratchDir, "nevr-server-smoke-")
	if err != nil {
		return "", fmt.Errorf("create isolated server config directory: %w", err)
	}
	defer os.RemoveAll(configDir)
	// -config-path is consumed by LoadLocalConfigHook, which loads a JSON game
	// config from that exact path.  NevrCfg separately derives config.yaml from
	// the custom config's directory, so both files must be siblings.
	configPath := filepath.Join(configDir, "config.json")
	const gameConfig = `{
  "loginservice_host": "http://127.0.0.1:1/login",
  "configservice_host": "http://127.0.0.1:1/config",
  "apiservice_host": "http://127.0.0.1:1/api",
  "api_host": "http://127.0.0.1:1/api",
  "transactionservice_host": "http://127.0.0.1:1/transaction",
  "matchingservice_host": "ws://127.0.0.1:1/matchmaking",
  "serverdb_host": "ws://127.0.0.1:1/serverdb",
  "graph_host": "http://127.0.0.1:1/graph",
  "graphservice_host": "http://127.0.0.1:1/graph"
}
`
	if err := os.WriteFile(configPath, []byte(gameConfig), 0600); err != nil {
		return "", fmt.Errorf("write isolated game config: %w", err)
	}

	nevrConfigPath := filepath.Join(configDir, "config.yaml")
	const nevrConfig = `services:
  socket_uri: "ws://127.0.0.1:1/nevr-server-smoke"
  serverdb: "ws://127.0.0.1:1/serverdb"
auth:
  http_uri: "http://127.0.0.1:1"
  http_key: "local-smoke-only"
identity:
  discord_id: "000000000000000001"
  password: "local-smoke-only"
`
	if err := os.WriteFile(nevrConfigPath, []byte(nevrConfig), 0600); err != nil {
		return "", fmt.Errorf("write isolated NEVR config: %w", err)
	}

	// N76 requires at least 45 seconds of patience before a server may be
	// declared hung.  Sixty seconds accommodates the game's splash phase while
	// keeping a broken opt-in smoke bounded.
	ctx, cancel := context.WithTimeout(context.Background(), serverStartupTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "wine", "./echovr.exe", "-server", "-noconsole", "-noaudio",
		"-config-path", configPath)
	cmd.Dir = binDir
	cmd.Env = append(os.Environ(), "DISPLAY=:101", "WINEPREFIX="+prefix)

	var output lockedBuffer
	cmd.Stdout = &output
	cmd.Stderr = &output
	if err := cmd.Start(); err != nil {
		return output.String(), fmt.Errorf("start echovr.exe: %w", err)
	}

	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()

	// These lines are emitted by runtime code, not inferred from a port scan:
	// ws_bridge.cpp emits the proxy marker and gameserver.cpp emits the IServerLib
	// initialization marker.  Requiring both prevents a splash-only process from
	// being mistaken for a running dedicated server.
	readyMarkers := []string{
		"[NEVR.PATCH] Successfully loaded custom config from:",
		"[NEVR.WS] Proxy listening on ws://127.0.0.1:",
		"[NEVR.GAMESERVER] Initialized game server",
	}
	ticker := time.NewTicker(200 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case err := <-done:
			return output.String(), fmt.Errorf("server exited before readiness: %w", err)
		case <-ctx.Done():
			// CommandContext terminates the Wine process.  Reap it before returning
			// so the test does not leave a game process behind after a startup failure.
			<-done
			return output.String(), fmt.Errorf("server did not reach readiness within %s", serverStartupTimeout)
		case <-ticker.C:
			current := output.String()
			if !containsAll(current, readyMarkers) {
				continue
			}

			// The server installs a SIGTERM path specifically to avoid invisible
			// modal dialogs.  This bounded shutdown check exercises that real path;
			// a non-zero Wait result is expected when Wine reports SIGTERM.
			if err := cmd.Process.Signal(syscall.SIGTERM); err != nil {
				return output.String(), fmt.Errorf("signal ready server: %w", err)
			}
			select {
			case <-done:
				return output.String(), nil
			case <-time.After(serverShutdownTimeout):
				if err := cmd.Process.Kill(); err != nil {
					return output.String(), fmt.Errorf("server ignored shutdown signal and kill failed: %w", err)
				}
				<-done
				return output.String(), fmt.Errorf("server did not exit within %s of shutdown signal", serverShutdownTimeout)
			}
		}
	}
}

func containsAll(output string, markers []string) bool {
	for _, marker := range markers {
		if !strings.Contains(output, marker) {
			return false
		}
	}
	return true
}

func TestServerStartsWithoutFatalError(t *testing.T) {
	output := requireLiveServer(t)
	if strings.Contains(output, "[NEVR.FATAL]") {
		t.Fatalf("server emitted the ServerFatal marker\n%s", output)
	}
}

func TestServerWebSocketBridgeBound(t *testing.T) {
	output := requireLiveServer(t)
	if !strings.Contains(output, "[NEVR.WS] Proxy listening on ws://127.0.0.1:") {
		t.Fatalf("WebSocket bridge bind marker missing\n%s", output)
	}
}

func TestServerCdnSkipped(t *testing.T) {
	output := requireLiveServer(t)
	// AssetCDN::Initialize logs in the [NEVR.CDN] namespace before starting its
	// fetch thread.  boot.cpp only calls that initializer when !g_isServer.
	if strings.Contains(output, "[NEVR.CDN]") {
		t.Fatalf("server emitted CDN activity despite the server-mode CDN gate\n%s", output)
	}
}

func TestServerModulesInitialized(t *testing.T) {
	output := requireLiveServer(t)
	for _, marker := range []string{
		"[NEVR.MODULE] Loaded: platform_compat (API v",
		"[NEVR.MODULE] Loaded: token_auth (API v",
	} {
		if !strings.Contains(output, marker) {
			t.Fatalf("module initialization marker %q missing\n%s", marker, output)
		}
	}
}

func TestServerNoBlockingDialogsOnShutdown(t *testing.T) {
	// runLiveServerSmoke sends the readiness-confirmed process SIGTERM and
	// fails unless it exits within serverShutdownTimeout.
	_ = requireLiveServer(t)
}

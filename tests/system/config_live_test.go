package system

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Config smoke tests intentionally have their own opt-in switch.  Unlike the
// normal client smoke, they replace every endpoint used during boot with
// loopback and create their config files outside the game installation.
const envLiveConfigTests = "NEVR_LIVE_CONFIG_TESTS"

const configSmokeScratchDir = "/var/tmp/work-nevr-runtime"

type configSmokeRun struct {
	output    string
	configDir string
	err       error
}

func requireLiveConfig(t *testing.T, withYaml bool) configSmokeRun {
	t.Helper()
	if os.Getenv(envLiveConfigTests) != "1" {
		t.Skip("set NEVR_LIVE_CONFIG_TESTS=1 to run the local Echo VR config smoke test")
	}

	run := runLiveConfigSmoke(withYaml)
	if run.err != nil {
		t.Fatalf("config smoke failed: %v\n%s", run.err, run.output)
	}
	return run
}

func runLiveConfigSmoke(withYaml bool) configSmokeRun {
	gameDir := getGameDir()
	binDir := filepath.Join(gameDir, "bin", "win10")
	prefix := os.Getenv("WINEPREFIX")
	if prefix == "" {
		prefix = filepath.Join(gameDir, ".wineprefix")
	}

	if err := os.MkdirAll(configSmokeScratchDir, 0700); err != nil {
		return configSmokeRun{err: fmt.Errorf("create config smoke scratch directory: %w", err)}
	}
	configDir, err := os.MkdirTemp(configSmokeScratchDir, "nevr-config-smoke-")
	if err != nil {
		return configSmokeRun{err: fmt.Errorf("create isolated config directory: %w", err)}
	}
	defer os.RemoveAll(configDir)

	// LoadLocalConfigHook consumes -config-path as an exact JSON file.  The
	// NEVR YAML discovery deliberately looks for config.yaml beside that file,
	// before trying the game installation's _local directories.
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
		return configSmokeRun{err: fmt.Errorf("write isolated game config: %w", err)}
	}

	if withYaml {
		// This is the authoritative NEVR config layer.  It repeats the endpoint
		// fence so future migrations from game JSON cannot accidentally make the
		// opt-in smoke contact an operator-configured or production service.
		const nevrConfig = `services:
  login: "http://127.0.0.1:1/login"
  config: "http://127.0.0.1:1/config"
  api: "http://127.0.0.1:1/api"
  api_legacy: "http://127.0.0.1:1/api"
  transaction: "http://127.0.0.1:1/transaction"
  matchmaking: "ws://127.0.0.1:1/matchmaking"
  serverdb: "ws://127.0.0.1:1/serverdb"
  serverdb_uri: "ws://127.0.0.1:1/serverdb"
  graph: "http://127.0.0.1:1/graph"
  graph_service: "http://127.0.0.1:1/graph"
  socket_uri: "ws://127.0.0.1:1/nevr-config-smoke"
auth:
  http_uri: "http://127.0.0.1:1"
  http_key: "local-config-smoke-only"
  server_key: "local-config-smoke-only"
identity:
  discord_id: "000000000000000001"
  password: "local-config-smoke-only"
network:
  external_ip: "127.0.0.1"
  internal_ip: "127.0.0.1"
  upnp: "false"
  upnp_port: "1"
`
		if err := os.WriteFile(filepath.Join(configDir, "config.yaml"), []byte(nevrConfig), 0600); err != nil {
			return configSmokeRun{err: fmt.Errorf("write isolated NEVR config: %w", err)}
		}
	}

	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	// -windowed is the regular desktop-client path (not spectator stream); it
	// implies -noovr in PreflightRuntimeBootstrap.  -noaudio keeps this smoke
	// silent for the operator.
	cmd := exec.CommandContext(ctx, "wine", "./echovr.exe", "-windowed", "-noaudio",
		"-config-path", configPath)
	cmd.Dir = binDir
	cmd.Env = append(os.Environ(), "DISPLAY=:101", "WINEPREFIX="+prefix)
	output, err := cmd.CombinedOutput()
	if ctx.Err() == context.DeadlineExceeded {
		// A healthy client remains open.  The assertions inspect the completed
		// runtime boot markers accumulated before CommandContext stops it.
		err = nil
	}
	return configSmokeRun{output: string(output), configDir: configDir, err: err}
}

func TestConfig_CustomConfigLoaded(t *testing.T) {
	run := requireLiveConfig(t, true)
	for _, marker := range []string{
		"[NEVR.PATCH] Successfully loaded custom config from:",
		"[NEVR.CONFIG] config.yaml loaded from:",
		"[NEVR.BOOT] runtime bootstrap complete",
	} {
		if !strings.Contains(run.output, marker) {
			t.Fatalf("custom config load marker %q missing\n%s", marker, run.output)
		}
	}
	// The same marker could otherwise come from the installation's _local
	// fallback.  The unique MkdirTemp basename is preserved in Wine's Z:\\ path,
	// so it proves discovery selected the explicit config-path sibling.
	if !strings.Contains(configYamlLogLine(run.output), filepath.Base(run.configDir)) {
		t.Fatalf("config.yaml load did not name the explicit config-path sibling\n%s", run.output)
	}
}

func TestConfig_MissingSiblingConfigGraceful(t *testing.T) {
	// The explicit config directory intentionally contains config.json only.
	// service_config.cpp then either emits its no-config/defaults marker or,
	// when the installation has a separate _local/config.yaml, reports that
	// fallback.  Both outcomes are source-defined graceful handling of the
	// missing sibling; neither may prevent the custom JSON config or boot from
	// completing.
	run := requireLiveConfig(t, false)
	for _, marker := range []string{
		"[NEVR.PATCH] Successfully loaded custom config from:",
		"[NEVR.BOOT] runtime bootstrap complete",
	} {
		if !strings.Contains(run.output, marker) {
			t.Fatalf("missing-sibling graceful marker %q missing\n%s", marker, run.output)
		}
	}
	if strings.Contains(run.output, "[NEVR.FATAL]") {
		t.Fatalf("missing config.yaml sibling caused a fatal error\n%s", run.output)
	}
	if !strings.Contains(run.output, "[NEVR.CONFIG] no config.yaml found") &&
		!strings.Contains(run.output, "[NEVR.CONFIG] config.yaml loaded from:") {
		t.Fatalf("expected missing-config defaults or fallback discovery marker\n%s", run.output)
	}
	if line := configYamlLogLine(run.output); line != "" && strings.Contains(line, filepath.Base(run.configDir)) {
		t.Fatalf("missing sibling unexpectedly appeared in YAML discovery\n%s", run.output)
	}
}

func configYamlLogLine(output string) string {
	const marker = "[NEVR.CONFIG] config.yaml loaded from:"
	start := strings.Index(output, marker)
	if start < 0 {
		return ""
	}
	line := output[start:]
	if end := strings.IndexByte(line, '\n'); end >= 0 {
		return line[:end]
	}
	return line
}

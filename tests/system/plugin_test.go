package system

import (
	"strings"
	"testing"
)

// The configured integration plugin is deliberately used here instead of a
// directory glob: the runtime's contract is that config.yaml is authoritative.
func TestPluginLoadsAndInitializes(t *testing.T) {
	output := requireLiveClient(t)
	for _, marker := range []string{
		"[NEVR.PLUGIN] 1 plugin(s) configured",
		"[NEVR.PLUGIN] Loaded: nevr_debug_lockout",
		"via Init",
	} {
		if !strings.Contains(output, marker) {
			t.Fatalf("missing plugin lifecycle marker %q\n%s", marker, output)
		}
	}
}

func TestPluginHostFrameDispatchIsAlive(t *testing.T) {
	output := requireLiveClient(t)
	if !strings.Contains(output,
		"per-frame tick ALIVE — plugin/module OnFrame now dispatched (host=client)") {
		t.Fatalf("host did not report plugin OnFrame dispatch\n%s", output)
	}
}

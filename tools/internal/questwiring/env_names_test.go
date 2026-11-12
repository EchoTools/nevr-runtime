package questwiring

import (
	"os/exec"
	"regexp"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// #2189: the Quest checks' environment variables are NEVR_ names, and the frame sensor's probe symbols are the ones its test
// expects (deleted_plugin_name_test.go keeps the deleted plugin's word out of both). docs/standards/naming.md: an environment variable is NEVR_<NAME>, so the switch that lets a combat check skip
// its qemu run is NEVR_SKIP_QEMU (src/quest/combat/check/*.sh), and the switch that makes a host test dump its module's log
// records is NEVR_TEST_VERBOSE (src/quest/tests/dronekill_test.cpp, dronemuzzle_test.cpp). The assembly probes of the frame
// sensor (src/quest/tests/frames_asm_probe_real.cpp) define the symbols the sensor test names
// (tests/quest/frames_asm_probes_test.go): a symbol named on one side only is an undefined symbol at link time, or a sensor
// that checks nothing.

const envNamesSelf = "tools/internal/questwiring/env_names_test.go"

var (
	combatChecks = []string{"dronemuzzle", "headset_gates", "powerboost"}
	verboseTests = []string{"dronekill_test.cpp", "dronemuzzle_test.cpp"}
	probeSymbols = []string{
		"probe_dm_tp_ret", "probe_dm_tp_next", "probe_dm_fire_ret", "probe_dm_fire_next", "probe_pb_cone_ret", "probe_pb_resolve_real",
		"probe_dm_teleport", "probe_dm_fire", "probe_pb_cone_at", "probe_dm_tp_thunk", "probe_dm_fire_thunk", "probe_pb_resolve_thunk",
	}
	probeWordRE = regexp.MustCompile(`"(probe_[a-z_]+)"`)
)

// trackedFiles lists the files git tracks under paths (relative to the repository root), as git ls-files names them.
func trackedFiles(t *testing.T, paths ...string) []string {
	t.Helper()
	cmd := exec.CommandContext(t.Context(), "git", append([]string{"ls-files", "-z", "--"}, paths...)...)
	cmd.Dir = root(t)
	cmd.Env = testkit.Env(t)
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("git ls-files %v: %v", paths, err)
	}
	var files []string
	for _, f := range strings.Split(string(out), "\x00") {
		if f != "" {
			files = append(files, f)
		}
	}
	return files
}

func TestEachCombatCheckReadsTheSkipVariableAndSaysSo(t *testing.T) {
	t.Parallel()
	for _, name := range combatChecks {
		text := read(t, "src/quest/combat/check/"+name+".sh")
		for _, want := range []string{
			`"${NEVR_SKIP_QEMU:-0}" = 1`,
			"SKIPPED (NEVR_SKIP_QEMU=1): qemu-aarch64 not found",
			"(NEVR_SKIP_QEMU=1 to skip explicitly)",
		} {
			if !strings.Contains(text, want) {
				t.Errorf("%s: missing %q", name, want)
			}
		}
	}
}

func TestEachVerboseTestReadsTheVerboseVariable(t *testing.T) {
	t.Parallel()
	for _, name := range verboseTests {
		if !strings.Contains(read(t, "src/quest/tests/"+name), `std::getenv("NEVR_TEST_VERBOSE")`) {
			t.Errorf("%s does not read NEVR_TEST_VERBOSE", name)
		}
	}
}

func TestTheProbeDefinesEachProbeSymbol(t *testing.T) {
	t.Parallel()
	text := read(t, "src/quest/tests/frames_asm_probe_real.cpp")
	for _, sym := range probeSymbols {
		if !regexp.MustCompile(`\b` + regexp.QuoteMeta(sym) + `\b`).MatchString(text) {
			t.Errorf("frames_asm_probe_real.cpp does not define %s", sym)
		}
	}
}

func TestTheSensorTestExpectsTheRenamedEntries(t *testing.T) {
	t.Parallel()
	text := read(t, "tests/quest/frames_asm_probes_test.go")
	for _, sym := range []string{"probe_dm_tp_thunk", "probe_dm_fire_thunk", "probe_pb_resolve_thunk"} {
		if !regexp.MustCompile(`\b` + sym + `\b`).MatchString(text) {
			t.Errorf("frames_asm_probes_test.go does not expect %s", sym)
		}
	}
}

func TestEveryStringTheProbeAsmNamesIsADefinedSymbol(t *testing.T) {
	t.Parallel()
	text := read(t, "src/quest/tests/frames_asm_probe_real.cpp")
	seen := map[string]bool{}
	for _, m := range probeWordRE.FindAllStringSubmatch(text, -1) {
		seen[m[1]] = true
		if !contains(probeSymbols, m[1]) {
			t.Errorf("the asm names %s, which is not in the probe list", m[1])
		}
		if !regexp.MustCompile(`\b` + m[1] + `\s*(=|\()`).MatchString(text) {
			t.Errorf("the asm names %s, which the probe file does not define", m[1])
		}
	}
	if len(seen) != 9 {
		t.Errorf("the asm bodies name %d symbols as string literals, want 9", len(seen))
	}
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

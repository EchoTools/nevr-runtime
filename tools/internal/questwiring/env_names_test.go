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

// #2242: the stock PC data folder (NEVR_STOCK_DATA, docs/reference/content-packs.md) is a per-machine input. The justfile reads it
// from the environment with no default path (AGENTS.md, "Nothing that only one machine has"), the recipes whose test runs reach a
// test that reads the folder pass it on, and .env.example lists it empty. The tests that need the folder skip without it
// (tools/cmd/content-pack-build, tools/internal/radpack). These checks pin that wiring, which the tests themselves cannot see.

const (
	stockDataDeclaration = `stock_data := env("NEVR_STOCK_DATA", "")`
	stockDataPassThrough = `NEVR_STOCK_DATA="{{ stock_data }}"`
)

// stockDataRecipes are the recipes that run a Go test reading the folder: test-changed (the map's Go packages), test-tools (the
// tool suite's Go half) and verify (its unittest line runs the same Go half).
var stockDataRecipes = []string{"test-changed", "test-tools", "verify"}

var stockDataDeclarationLine = regexp.MustCompile(`(?m)^stock_data\s*:=.*$`)

func TestTheStockDataVariableIsReadFromTheEnvironmentWithNoDefaultPath(t *testing.T) {
	t.Parallel()
	text := read(t, "justfile")
	decls := stockDataDeclarationLine.FindAllString(text, -1)
	if len(decls) != 1 || decls[0] != stockDataDeclaration {
		t.Errorf("justfile declares stock_data as %q, want exactly %q (an environment read with an empty default)", decls, stockDataDeclaration)
	}
	for _, line := range strings.Split(text, "\n") {
		if strings.Contains(line, "NEVR_STOCK_DATA=") && !strings.Contains(line, stockDataPassThrough) {
			t.Errorf("justfile sets NEVR_STOCK_DATA other than through the variable: %s", strings.TrimSpace(line))
		}
	}
}

func TestTheRecipesThatRunTheStockDataTestsPassTheVariableThrough(t *testing.T) {
	t.Parallel()
	text := read(t, "justfile")
	for _, name := range stockDataRecipes {
		if body := stockDataRecipeBody(t, text, name); !strings.Contains(body, stockDataPassThrough) {
			t.Errorf("recipe %s does not pass NEVR_STOCK_DATA on to the tests it runs", name)
		}
	}
}

func TestEnvExampleListsTheVariableEmpty(t *testing.T) {
	t.Parallel()
	found := false
	for _, line := range strings.Split(read(t, ".env.example"), "\n") {
		if line == "NEVR_STOCK_DATA=" {
			found = true
		}
	}
	if !found {
		t.Error(".env.example does not list NEVR_STOCK_DATA= (empty: the stock-data tests skip)")
	}
}

// stockDataRecipeBody is the text of the recipe named name: its header and the lines below it that are indented or blank.
func stockDataRecipeBody(t *testing.T, text, name string) string {
	t.Helper()
	header := regexp.MustCompile(`(?m)^` + regexp.QuoteMeta(name) + `(\s[^\n]*)?:[^\n]*$`)
	loc := header.FindStringIndex(text)
	if loc == nil {
		t.Fatalf("justfile has no recipe %s", name)
	}
	lines := strings.Split(text[loc[1]:], "\n")
	var body []string
	for _, line := range lines[1:] {
		if line != "" && line[0] != ' ' && line[0] != '\t' {
			break
		}
		body = append(body, line)
	}
	return strings.Join(body, "\n")
}

package questwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #2187: the EC client and NEUTRON effects are the runtime's. The host tests (ec_test, ec_page_test, ecfx_test, combat_install_test) drive the
// behaviour; these pin what they cannot see: that the q15t plugin installs on none of ec's slots (one owner per slot on a q15t-variant APK),
// that ec hooks only through thunks, that its rows are in the table in the plugin's order, and that its keys are on the schema and the example.

// Every slot ec_targets.h pins, as the plugin would have to spell it, and WriteLog's own slot, which the plugin hooked and nothing hooks now.
var ecSlots = []string{"0x36d3e88", "0x36db3d0", "0x36cb1b0", "0x36ea1b0", "0x36f1878", "0x6e1238", "0x731ee8", "0x37af5f8"}
var ecPluginSlots = append([]string{"0x36fd398"}, ecSlots...)

func TestThePluginInstallsOnNoneOfTheEcSlots(t *testing.T) {
	targets := read(t, "src/quest/combat/ec_targets.h")
	for _, slot := range ecSlots {
		if !strings.Contains(strings.ToLower(targets), slot) {
			t.Errorf("ec_targets.h must pin %s", slot)
		}
	}
	files, err := filepath.Glob(filepath.Join(root(t), "src/quest/plugins/q15t/src/*"))
	if err != nil || len(files) == 0 {
		t.Fatalf("no plugin sources: %v", err)
	}
	for _, f := range files {
		raw, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		code := strings.ToLower(stripComments(string(raw)))
		for _, slot := range ecPluginSlots {
			if strings.Contains(code, slot) {
				t.Errorf("%s names ec's slot %s: the slot has one owner, the runtime's ec", filepath.Base(f), slot)
			}
		}
		for _, name := range []string{"\"recvfrom\"", "\"sendto\"", "writelogv", "re33_ec_page", "neutronfx", "ec_words"} {
			if strings.Contains(code, name) {
				t.Errorf("%s names %s: the q15t plugin must not carry the EC client", filepath.Base(f), name)
			}
		}
	}
	for _, gone := range []string{
		"src/quest/plugins/q15t/src/ec.cpp", "src/quest/plugins/q15t/src/ec.h", "src/quest/plugins/q15t/src/ec_core.h",
		"src/quest/plugins/q15t/src/ec_fld.h", "src/quest/plugins/q15t/src/ecfx.cpp", "src/quest/plugins/q15t/src/ecfx.h",
		"src/quest/plugins/q15t/src/ecfx_core.h", "src/quest/tests/q15t/mod/ec.cmake", "src/quest/tests/q15t/mod/ecfx.cmake",
	} {
		if exists(t, gone) {
			t.Errorf("%s must not exist: ec and ecfx are the runtime's", gone)
		}
	}
	cmake := read(t, "src/quest/plugins/q15t/CMakeLists.txt")
	for _, name := range []string{"Q15T_EC ", "Q15T_ECFX", "Q15T_EC_DEFAULT_WORDS", " EC ECFX"} {
		if strings.Contains(cmake, name) {
			t.Errorf("the q15t plugin's CMakeLists.txt must not carry %s", name)
		}
	}
}

func TestEcHooksOnlyThroughThunksAndTheManager(t *testing.T) {
	code := stripComments(read(t, "src/quest/combat/ec.cpp") + read(t, "src/quest/combat/ecfx.cpp"))
	for _, forbidden := range []string{"SwapWord", "InstallImportHook", "InstallAsmEntry", "mprotect", "PROT_WRITE", "thread_local"} {
		if strings.Contains(code, forbidden) {
			t.Errorf("ec / ecfx must install only through InstallThunk; they name %s", forbidden)
		}
	}
	if n := len(regexp.MustCompile(`NEVR_HOOK_RECORD\(`).FindAllString(code, -1)); n != 7 {
		t.Errorf("want seven hook records (recvfrom, sendto, three WriteLogV, Reload, Unload), got %d", n)
	}
}

func TestTheEcRowsComeAfterRefreshRateAndBeforeTheGates(t *testing.T) {
	table := read(t, "src/quest/combat/combat_install.cpp")
	rr, ec, ecfx, gates := strings.Index(table, `Module{"refresh_rate"`), strings.Index(table, `Module{"ec"`), strings.Index(table, `Module{"ecfx"`),
		strings.Index(table, `Module{"gates"`)
	if rr < 0 || ec < rr || ecfx < ec || gates < ecfx {
		t.Errorf("the rows must be refresh_rate, ec, ecfx, gates in that order (got %d %d %d %d)", rr, ec, ecfx, gates)
	}
}

func TestEveryEcKeyIsOnTheSchemaAndTheExample(t *testing.T) {
	schema := read(t, "src/core/nevr_config.cpp")
	example := read(t, "docs/reference/example-config.yaml")
	for _, key := range []string{"ec", "ec_words", "ecfx"} {
		if !strings.Contains(schema, `"`+key+`"`) {
			t.Errorf("KnownSections must list combat.%s", key)
		}
		if !strings.Contains(example, "  # "+key+":") {
			t.Errorf("docs/reference/example-config.yaml must show combat.%s", key)
		}
	}
}

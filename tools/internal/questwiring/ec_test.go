package questwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2187: the EC client and NEUTRON effects are the runtime's. The host tests (ec_test, ec_page_test, ecfx_test, combat_install_test) drive the
// behaviour; these pin what they cannot see: that ec_targets.h pins every slot ec hooks, that ec hooks only through thunks, that its rows are in the table in the plugin's order, and that its keys are on the schema and the example.

// Every slot ec_targets.h pins.
var ecSlots = []string{"0x36d3e88", "0x36db3d0", "0x36cb1b0", "0x36ea1b0", "0x36f1878", "0x6e1238", "0x731ee8", "0x37af5f8"}

func TestEcTargetsPinsEveryEcSlot(t *testing.T) {
	targets := read(t, "src/quest/combat/ec_targets.h")
	for _, slot := range ecSlots {
		if !strings.Contains(strings.ToLower(targets), slot) {
			t.Errorf("ec_targets.h must pin %s", slot)
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
	if n := len(regexp.MustCompile(`NEVR_HOOK_RECORD\(`).FindAllString(code, -1)); n != 8 {
		t.Errorf("want eight hook records (recvfrom, sendto, three WriteLogV, Reload, Unload, LobbySettingsCB #2333), got %d", n)
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

package questwiring

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// #2178: the crash and timing guards (fxtrail, voipdrop, custctx, droneguard, peerclamp) are the runtime's, and the q15t plugin installs on
// none of their slots. The host tests (fxtrail_test, voipdrop_test, custctx_test, droneguard_test, peerclamp_test, level_scope_test) drive the
// behaviour; these pin what they cannot see: who owns each slot, the table's order, the scope each guard has in arena, the keys.

type guard struct {
	name    string
	symbols []string // the dynsym names the module hooks
	slots   []string // the JUMP_SLOT vaddrs, spelled as the plugin spelled them
	scope   string   // the level_scope answer the handler asks, or "" when it acts in every level on purpose
	handles int      // the GotHook handles the module declares (hook_budget.h)
	row     string   // the registration table row, up to the end step
}

var guards = []guard{
	{"fxtrail", []string{"CParticleEffectCS10EffectTime"}, []string{"0x36e7b98"}, "scope::InCombatMap()", 1,
		`Module{"fxtrail", nullptr, &FxtrailNormal, &fxtrail::NoteEnd}`},
	{"voipdrop", []string{"CR15NetVoipReceiverCS11ReceiveVoip"}, []string{"0x36c5d80"}, "", 1,
		`Module{"voipdrop", nullptr, &VoipdropNormal, &voipdrop::NoteEnd}`},
	{"custctx", []string{"CR15NetCustomizationRenderer12LoadMeshData", "CR15NetCustomizationRenderer14DestroyContext", "SR15NetCustomDrawDataEE4GrowEv"},
		[]string{"0x36cf6f0", "0x36c22f8", "0x36ed7f0"}, "", 3, `Module{"custctx", nullptr, &CustctxNormal, &custctx::NoteEnd}`},
	{"droneguard", []string{"CR15NetFollowPlayerCS6Follow"}, []string{"0x36f1218"}, "", 1,
		`Module{"droneguard", nullptr, &DroneguardNormal, &droneguard::NoteEnd}`},
	{"peerclamp", []string{"SBroadcasterData12CleanupPeers"}, []string{"0x36efad8"}, "scope::InCombatMapOrLobby()", 1,
		`Module{"peerclamp", nullptr, &PeerclampNormal, &peerclamp::NoteEnd}`},
}

func TestTheGuardsAreNormalPhaseRowsInThePluginsOrderAfterTheRefreshRate(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	prev := `Module{"refresh_rate", nullptr, &RefreshRateNormal}`
	for _, g := range guards {
		before(t, table, prev, g.row)
		prev = g.row
	}
	// each row reads its own switch from the configuration the constructor's step handed over
	for _, want := range []string{
		"fxtrail::Install(g_config.fxtrail.on)", "voipdrop::Install(g_config.voipdrop.on)", "custctx::Install(g_config.custctx.on)",
		"droneguard::Install(g_config.droneguard.on, g_config.droneguardStage1.on)", "peerclamp::Install(g_config.peerclamp.on)",
	} {
		if !strings.Contains(table, want) {
			t.Errorf("combat_install.cpp lacks %q", want)
		}
	}
}

func TestThePluginInstallsOnNoSlotOfTheGuardsAndTheirFilesAreGone(t *testing.T) {
	for _, g := range guards {
		for _, f := range []string{
			"src/quest/plugins/q15t/src/" + g.name + ".h", "src/quest/plugins/q15t/src/" + g.name + ".cpp",
			"src/quest/tests/q15t/" + g.name + "_test.cpp", "src/quest/tests/q15t/mod/" + g.name + ".cmake",
			"src/quest/tests/q15t/mutants.d/" + g.name + ".sh", "docs/quest/q15t/modules/" + g.name + ".md",
		} {
			if exists(t, f) {
				t.Errorf("%s still exists", f)
			}
		}
		for _, f := range []string{"src/quest/combat/" + g.name + ".h", "src/quest/combat/" + g.name + ".cpp", "src/quest/combat/" + g.name + "_targets.h",
			"src/quest/tests/" + g.name + "_test.cpp", "src/quest/combat/mutants.d/" + g.name + ".sh", "docs/quest/combat/" + g.name + ".md"} {
			if !exists(t, f) {
				t.Errorf("%s is missing", f)
			}
		}
	}
	entries, err := os.ReadDir(filepath.Join(root(t), "src/quest/plugins/q15t/src"))
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		ext := filepath.Ext(e.Name())
		if ext != ".cpp" && ext != ".h" && ext != ".inc" {
			continue
		}
		code := stripComments(read(t, "src/quest/plugins/q15t/src/"+e.Name()))
		for _, g := range guards {
			for _, bad := range append(append([]string{}, g.symbols...), g.slots...) {
				if strings.Contains(code, bad) {
					t.Errorf("%s names %s, which is the runtime's %s", e.Name(), bad, g.name)
				}
			}
		}
	}
	cmake := read(t, "src/quest/plugins/q15t/CMakeLists.txt") + read(t, "src/quest/plugins/q15t/src/plugin.cpp")
	for _, bad := range []string{"Q15T_FXTRAIL", "Q15T_VOIPDROP", "Q15T_CUSTCTX", "Q15T_DRONEGUARD", "Q15T_PEERCLAMP", "InstallFxTrail", "InstallVoipDrop",
		"InstallCustCtx", "InstallDroneGuard", "InstallPeerClamp"} {
		if strings.Contains(cmake, bad) {
			t.Errorf("the plugin still names %s", bad)
		}
	}
}

func TestOnlyFxtrailAndPeerclampAreScopedByLevelAndTheOthersActInEveryLevelOnPurpose(t *testing.T) {
	for _, g := range guards {
		code := stripComments(read(t, "src/quest/combat/"+g.name+".cpp"))
		header := read(t, "src/quest/combat/"+g.name+".h")
		asks := strings.Contains(code, "level_scope.h") || strings.Contains(code, "scope::")
		if g.scope == "" {
			if asks {
				t.Errorf("%s is a crash-only guard and acts in every level, but it asks the level scope", g.name)
			}
			if !strings.Contains(strings.ToLower(header), "every level") {
				t.Errorf("%s.h does not say that it acts in every level and why", g.name)
			}
			continue
		}
		if !strings.Contains(code, g.scope) {
			t.Errorf("%s.cpp does not ask %s", g.name, g.scope)
		}
		if !strings.Contains(header, "Arena") {
			t.Errorf("%s.h has no Arena paragraph", g.name)
		}
		// the scope is asked before any game memory is read in the handler that asks it: arena pays one atomic load and reads nothing
		at := strings.Index(code, g.scope)
		start := strings.LastIndex(code[:at], "\n}\n") // the end of the function before the handler: helpers above it are not the handler
		for _, needle := range []string{"SafeRead(", "SafeRead<"} {
			if r := strings.Index(code[start+1:at], needle); r >= 0 {
				t.Errorf("%s reads game memory (%s) in the handler before it asks the level scope", g.name, needle)
			}
		}
	}
}

func TestEachGuardHasItsKeyReadKnownLoggedAndDocumented(t *testing.T) {
	reader, schema := read(t, "src/quest/sentinel/quest_config.cpp"), read(t, "src/core/nevr_config.cpp")
	line, example := read(t, "src/quest/combat/combat_install.cpp"), read(t, "docs/reference/example-config.yaml")
	for _, key := range []string{"fxtrail", "voipdrop", "custctx", "droneguard", "droneguard_stage1", "peerclamp"} {
		for what, text := range map[string]string{"reader": reader, "schema": schema, "line": line} {
			if !strings.Contains(text, `"`+key+`"`) && !strings.Contains(text, `"combat.`+key+`"`) {
				t.Errorf("%s does not name %s", what, key)
			}
		}
		if !strings.Contains(example, "\n  # "+key+": ") {
			t.Errorf("the example config does not document %s", key)
		}
	}
	// the second stage of droneguard is the one key that defaults off, in the struct and in the reader
	if !strings.Contains(read(t, "src/quest/sentinel/quest_config.h"), "CombatSwitch droneguardStage1{false, Source::kAbsent}") {
		t.Error("combat.droneguard_stage1 does not default to off")
	}
	if !strings.Contains(reader, "ReadCombatSwitch(cfg, kCombatDroneguardStage1Key, r, /*fallback=*/false)") {
		t.Error("the reader's fallback for combat.droneguard_stage1 is not off")
	}
}

func TestEachGuardHasItsHookBudgetRowAndNoSwitchFileIsRead(t *testing.T) {
	budget := read(t, "src/quest/sentinel/hook_budget.h")
	for _, g := range guards {
		want := `{"combat/` + g.name + `.cpp", ` + string(rune('0'+g.handles)) + `, 0}`
		if !strings.Contains(budget, want) {
			t.Errorf("hook_budget.h lacks %s", want)
		}
		code := stripComments(read(t, "src/quest/combat/"+g.name+".cpp"))
		// a switch file of the plugin is told about (legacy_switch.h) and never read as a switch: the module opens no file itself
		for _, bad := range []string{"fopen(", "access(", "ifstream"} {
			if strings.Contains(code, bad) {
				t.Errorf("%s.cpp calls %s: its switch is a combat: key", g.name, bad)
			}
		}
	}
}

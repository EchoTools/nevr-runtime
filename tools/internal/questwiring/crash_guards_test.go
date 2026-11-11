package questwiring

import (
	"path/filepath"
	"strings"
	"testing"
)

// #2178: the crash and timing guards (fxtrail, voipdrop, custctx, droneguard, peerclamp) are the runtime's, and the q15t plugin installs on
// none of their slots. The host tests (fxtrail_test, voipdrop_test, custctx_test, droneguard_test, peerclamp_test) drive the
// behaviour; these pin what they cannot see: who owns each slot, the table's order, the scope each guard has in arena, the keys.

type guard struct {
	name    string
	symbols []string // the dynsym names the module hooks
	slots   []string // the JUMP_SLOT vaddrs, spelled as the plugin spelled them
	scope   string   // the combat_scope answer the handler tests first, or "" when it acts in every level on purpose
	handles int      // the GotHook handles the module declares (hook_budget.h)
	row     string   // the registration table row, up to the end step
}

var guards = []guard{
	{"fxtrail", []string{"CParticleEffectCS10EffectTime"}, []string{"0x36e7b98"}, "scope::SeenCombatMap()", 1,
		`Module{"fxtrail", nullptr, &FxtrailNormal, "combat.fxtrail", &nevr_quest::CombatConfig::fxtrail, &fxtrail::NoteEnd}`},
	{"voipdrop", []string{"CR15NetVoipReceiverCS11ReceiveVoip"}, []string{"0x36c5d80"}, "", 1,
		`Module{"voipdrop", nullptr, &VoipdropNormal, "combat.voipdrop", &nevr_quest::CombatConfig::voipdrop, &voipdrop::NoteEnd}`},
	{"custctx", []string{"CR15NetCustomizationRenderer12LoadMeshData", "CR15NetCustomizationRenderer14DestroyContext", "SR15NetCustomDrawDataEE4GrowEv"},
		[]string{"0x36cf6f0", "0x36c22f8", "0x36ed7f0"}, "scope::SeenCombatOrLobby()", 3,
		`Module{"custctx", nullptr, &CustctxNormal, "combat.custctx", &nevr_quest::CombatConfig::custctx, &custctx::NoteEnd}`},
	{"droneguard", []string{"CR15NetFollowPlayerCS6Follow"}, []string{"0x36f1218"}, "", 1,
		`Module{"droneguard", nullptr, &DroneguardNormal, "combat.droneguard", &nevr_quest::CombatConfig::droneguard, &droneguard::NoteEnd}`},
	{"peerclamp", []string{"SBroadcasterData12CleanupPeers"}, []string{"0x36efad8"}, "scope::SeenCombatOrLobby()", 1,
		`Module{"peerclamp", nullptr, &PeerclampNormal, "combat.peerclamp", &nevr_quest::CombatConfig::peerclamp, &peerclamp::NoteEnd}`},
}

func TestTheGuardsAreNormalPhaseRowsInThePluginsOrderAfterTheScopeRowAndSendguardAndBeforeThePfxDecisions(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	before(t, table, `Module{"scope", nullptr, &ScopeNormal}`, guards[0].row)
	prev := `Module{"sendguard", nullptr, &SendguardNormal,`
	for _, g := range guards {
		before(t, table, prev, g.row)
		prev = g.row
	}
	// droneguard's second stage is a switch of its own: a row with no step, so the switch is one generated combat_config_switch record
	stage1 := `Module{"droneguard_stage1", nullptr, nullptr, "combat.droneguard_stage1", &nevr_quest::CombatConfig::droneguardStage1}`
	before(t, table, guards[3].row, stage1)
	before(t, table, stage1, guards[4].row)
	before(t, table, guards[4].row, `Module{"pfx", nullptr, &PfxNormal}`)
	// the scope's refresh is on while one of the readers of its answer is on, and these three are readers
	scopeRow := body(t, table, "ModuleResult ScopeNormal()")
	for _, want := range []string{"g_config.fxtrail.on", "g_config.peerclamp.on", "g_config.custctx.on"} {
		if !strings.Contains(scopeRow, want) {
			t.Errorf("ScopeNormal() does not list %s as a reader of the combat level answer", want)
		}
	}
	for _, not := range []string{"g_config.voipdrop.on", "g_config.droneguard.on"} {
		if strings.Contains(scopeRow, not) {
			t.Errorf("ScopeNormal() lists %s, which asks nothing of the combat level answer", not)
		}
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
	entries := pluginEntries(t, "src/quest/plugins/q15t/src")
	for _, e := range entries {
		ext := filepath.Ext(e.Name())
		if ext != ".cpp" && ext != ".h" && ext != ".inc" {
			continue
		}
		code := stripComments(pluginRead(t, "src/quest/plugins/q15t/src/"+e.Name()))
		for _, g := range guards {
			for _, bad := range append(append([]string{}, g.symbols...), g.slots...) {
				if strings.Contains(code, bad) {
					t.Errorf("%s names %s, which is the runtime's %s", e.Name(), bad, g.name)
				}
			}
		}
	}
	cmake := pluginRead(t, "src/quest/plugins/q15t/CMakeLists.txt") + pluginRead(t, "src/quest/plugins/q15t/src/plugin.cpp")
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
		asks := strings.Contains(code, "combat_scope.h") || strings.Contains(code, "scope::")
		if g.scope == "" {
			if asks {
				t.Errorf("%s is a crash-only guard and acts in every level, but it asks the combat level answer", g.name)
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
		// the scope is asked before any game memory is read in the handler that asks it: arena pays one relaxed load and reads nothing
		at := strings.Index(code, g.scope)
		start := strings.LastIndex(code[:at], "\n}\n") // the end of the function before the handler: helpers above it are not the handler
		for _, needle := range []string{"SafeRead(", "SafeRead<"} {
			if r := strings.Index(code[start+1:at], needle); r >= 0 {
				t.Errorf("%s reads game memory (%s) in the handler before it asks the combat level answer", g.name, needle)
			}
		}
		// the answer is believed only while its look is fresh: the combat body, a separate function, begins with LookIsFresh
		if !strings.Contains(code, "scope::LookIsFresh()") {
			t.Errorf("%s.cpp never checks that the look is fresh", g.name)
		}
	}
	// the two guards that need the answer to change refuse the install when it cannot (hudmeter's shape)
	for _, name := range []string{"fxtrail", "peerclamp"} {
		code := stripComments(read(t, "src/quest/combat/"+name+".cpp"))
		before(t, code, "scope::Live()", "lookup(sentinel::pinned::kLibR15")
		if !strings.Contains(code, `"scope_not_live"`) {
			t.Errorf("%s.cpp does not refuse with scope_not_live", name)
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

// custctx's destroy and Grow guards stop crashes and act in every level; only its pool refusal (a changed result of LoadMeshData) is scoped, and it is
// tested before any pool word is read.
func TestCustctxScopesTheRefusalAndNotTheDestroyAndGrowGuards(t *testing.T) {
	code := stripComments(read(t, "src/quest/combat/custctx.cpp"))
	for _, sig := range []string{"std::uint64_t DestroyHandler(", "GrowHandler("} {
		if b := body(t, code, sig); strings.Contains(b, "scope::") {
			t.Errorf("%s asks the combat level answer: that guard acts in every level", sig)
		}
	}
	load := body(t, code, "std::uint64_t LoadHandler(")
	before(t, load, "scope::SeenCombatOrLobby()", "PoolIsLowInScope(")
	if strings.Contains(load, "RtLow(") {
		t.Error("LoadHandler reads the pool itself: it must go through PoolIsLowInScope, which checks the look is fresh")
	}
	// the install does not depend on the scope: the destroy and Grow guards install when the answer cannot change
	install := body(t, code, "ModuleResult InstallAt(")
	if strings.Contains(install, `"scope_not_live"`) {
		t.Error("custctx refuses the install when the scope is not live: its destroy and Grow guards need no level")
	}
}

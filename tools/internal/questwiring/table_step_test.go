package questwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2177: the four resource table-step modules (meshstep, instmodel, shaderstep, tablestep) are the runtime's, scoped to a combat match, and the q15t
// plugin installs on none of their slots. The host tests (meshstep_test, instmodel_test, shaderstep_test, tablestep_test, shaderstep_tablestep_test)
// drive the behaviour; these pin what they cannot see: the registration order, that each handler asks the scope before it acts, that the scope is
// the shared level answer and not a copy, and what the plugin no longer holds.

var tableStepModules = []string{"meshstep", "instmodel", "shaderstep", "tablestep"}

func TestTheTableStepsRegisterInTheNormalPhaseInThePluginsOrderAfterTheTick(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	before(t, table, `Module{"tick", nullptr, &tick::InstallNormal}`, `Module{"meshstep", nullptr, &MeshstepNormal,`)
	before(t, table, `Module{"meshstep", nullptr, &MeshstepNormal,`, `Module{"tablestep", nullptr, &TablestepNormal,`)
	before(t, table, `Module{"tablestep", nullptr, &TablestepNormal,`, `Module{"instmodel", nullptr, &InstmodelNormal,`)
	before(t, table, `Module{"instmodel", nullptr, &InstmodelNormal,`, `Module{"shaderstep", nullptr, &ShaderstepNormal,`)
	for _, m := range tableStepModules {
		if strings.Contains(table, `Module{"`+m+`", &`) {
			t.Errorf("%s has an early step: it reads the game the tick verifies, which exists only after the early phase", m)
		}
	}
}

func TestEachTableStepIsAThunkInstalledThroughTheManagerWithAnOwnerAndABudgetRow(t *testing.T) {
	budget := read(t, "src/quest/sentinel/hook_budget.h")
	for _, m := range tableStepModules {
		code := stripComments(read(t, "src/quest/combat/"+m+".cpp"))
		for _, want := range []string{"sentinel::InstallThunk<Thunk>(g_hook, ", "NEVR_HOOK_RECORD(kRecord, Thunk, &Handler);"} {
			if !strings.Contains(code, want) {
				t.Errorf("%s.cpp lacks %q", m, want)
			}
		}
		for _, bad := range []string{"mprotect", "SwapWord", "InstallImportHook", "ModuleSwitchPresent"} {
			if strings.Contains(code, bad) {
				t.Errorf("%s.cpp names %s: a module installs only through the manager and reads no kill file", m, bad)
			}
		}
		if !strings.Contains(stripComments(read(t, "src/quest/combat/"+m+"_targets.h")), `.Owned("combat_`+m+`")`) {
			t.Errorf("%s's target names no owner", m)
		}
		if !strings.Contains(budget, `{"combat/`+m+`.cpp", 1, 0}`) {
			t.Errorf("no hook budget row for %s", m)
		}
		if !strings.Contains(code, `WarnLegacySwitch("combat_`+m+`_legacy_switch", "`+m+`.off", "combat.`+m+`: false")`) {
			t.Errorf("%s does not warn about the plugin's kill file", m)
		}
	}
}

func TestEachHandlerAsksTheScopeBeforeItActsAndTheGamesObjectIsTheDefault(t *testing.T) {
	for _, m := range tableStepModules {
		code := stripComments(read(t, "src/quest/combat/"+m+".cpp"))
		h := body(t, code, "Handler(Thunk::Fn original")
		if !strings.Contains(h, "table_step::CurrentScope()") {
			t.Errorf("%s's handler does not read the scope", m)
		}
		if !strings.Contains(h, "scope != table_step::Scope::kCombat") {
			t.Errorf("%s's handler does not leave every scope but a combat match to the game", m)
		}
	}
	mesh := body(t, stripComments(read(t, "src/quest/combat/meshstep.cpp")), "Handler(Thunk::Fn original")
	before(t, mesh, "scope != table_step::Scope::kCombat", "BuildMeshList(")
	before(t, mesh, "original(self, init)", "BuildMeshList(")
	// the modules that call the game's function do so before they touch its object
	for _, m := range []string{"instmodel", "shaderstep", "tablestep"} {
		h := body(t, stripComments(read(t, "src/quest/combat/"+m+".cpp")), "Handler(Thunk::Fn original")
		before(t, h, "original(", "ApplyS")
	}
}

func TestTheScopeIsTheSharedLevelAnswerAndArenaWinsOverACombatMapLoadedDuringATransition(t *testing.T) {
	ts := stripComments(read(t, "src/quest/combat/table_step.cpp"))
	if !regexp.MustCompile(`level::Classify\(game,\s*false,\s*&level,\s*true\)\s*==\s*level::Kind::kCombatOrLobby`).MatchString(ts) {
		t.Error("the scope is not combat_level.h's Classify with the lobby rule off and the gamespaces still loading included")
	}
	if !strings.Contains(ts, "tick::Game()") {
		t.Error("the scope does not read the game the combat tick verified")
	}
	for _, m := range append([]string{"table_step"}, tableStepModules...) {
		for _, f := range []string{"src/quest/combat/" + m + ".cpp", "src/quest/combat/" + m + ".h"} {
			if strings.Contains(stripComments(read(t, f)), "0x43e2da7914642604") {
				t.Errorf("%s carries its own copy of the level list", f)
			}
		}
	}
}

func TestThePluginHoldsNoneOfTheFourModulesAndTheirDocsMoved(t *testing.T) {
	cmake := read(t, "src/quest/plugins/q15t/CMakeLists.txt")
	plugin := read(t, "src/quest/plugins/q15t/src/plugin.cpp")
	for _, m := range tableStepModules {
		for _, p := range []string{
			"src/quest/plugins/q15t/src/" + m + ".h", "src/quest/plugins/q15t/src/" + m + ".cpp",
			"src/quest/tests/q15t/" + m + "_test.cpp", "src/quest/tests/q15t/mod/" + m + ".cmake", "docs/quest/q15t/modules/" + m + ".md",
		} {
			if exists(t, p) {
				t.Errorf("%s still exists", p)
			}
		}
		for _, p := range []string{"src/quest/combat/" + m + ".h", "src/quest/combat/" + m + ".cpp", "src/quest/combat/" + m + "_targets.h",
			"src/quest/tests/" + m + "_test.cpp", "docs/quest/combat/" + m + ".md", "src/quest/combat/mutants.d/" + m + ".sh"} {
			if !exists(t, p) {
				t.Errorf("%s is missing", p)
			}
		}
		upper := strings.ToUpper(m)
		for what, text := range map[string]string{"CMake": cmake, "plugin.cpp": plugin} {
			if strings.Contains(text, "Q15T_"+upper) || strings.Contains(text, `"`+m+`"`) || strings.Contains(text, "src/"+m+".cpp") {
				t.Errorf("the plugin's %s still names %s", what, m)
			}
		}
	}
	for _, bad := range []string{"InstallMeshStep", "InstallInstModel", "InstallShaderStep", "InstallTableStep"} {
		if strings.Contains(plugin, bad) {
			t.Errorf("plugin.cpp still calls %s", bad)
		}
	}
}

func TestMeshstepIsOffByDefaultInTheReaderToo(t *testing.T) {
	if !strings.Contains(read(t, "src/quest/sentinel/quest_config.cpp"), "ReadCombatSwitch(cfg, kCombatMeshstepKey, r, /*fallback=*/false)") {
		t.Error("the reader's fallback for combat.meshstep is not off")
	}
}

func TestEachTableStepSwitchIsReportedByARecordGeneratedFromTheTable(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	for _, m := range tableStepModules {
		if !strings.Contains(table, `"combat.`+m+`", &nevr_quest::CombatConfig::`+m+`}`) {
			t.Errorf("the %s row names no switch key and member", m)
		}
	}
	b := body(t, table, "void LogConfig(")
	if !strings.Contains(b, `"combat_config_switch"`) || !strings.Contains(b, "module.switchMember") {
		t.Error("LogConfig does not write the per-module switch records from the table")
	}
	if strings.Contains(table, "combat_config_table_steps") {
		t.Error("the table steps still have a line of their own")
	}
}

func TestEachTableStepKeyIsReadKnownLoggedAndDocumentedAndDefaultsOn(t *testing.T) {
	reader, schema := read(t, "src/quest/sentinel/quest_config.cpp"), read(t, "src/core/nevr_config.cpp")
	line, example := read(t, "src/quest/combat/combat_install.cpp"), read(t, "docs/reference/example-config.yaml")
	header := read(t, "src/quest/sentinel/quest_config.h")
	for _, key := range tableStepModules {
		for what, text := range map[string]string{"reader": reader, "schema": schema, "line": line} {
			if !strings.Contains(text, `"`+key+`"`) && !strings.Contains(text, `"combat.`+key+`"`) {
				t.Errorf("%s does not name %s", what, key)
			}
		}
		if !regexp.MustCompile(`\n  # ` + key + `: (false|true)\n`).MatchString(example) {
			t.Errorf("the example config does not document %s", key)
		}
		want := "CombatSwitch " + key + ";"
		if key == "meshstep" { // the plugin's meshstep never ran on a headset: off until a headset run shows the replacement work
			want = "CombatSwitch meshstep{false, Source::kAbsent};"
		}
		if !strings.Contains(header, want) {
			t.Errorf("CombatConfig lacks %q", want)
		}
	}
}

// combat_level's in-flight source is for the callers that act while resources are built; refresh_rate and pfx decide per frame on a level that is up
// and keep what they always read.
func TestOnlyTheTableStepsAskForTheGamespacesStillLoading(t *testing.T) {
	asked := regexp.MustCompile(`(?:Classify|Scan)\([^;]*\btrue\)`)
	for _, f := range []string{"src/quest/combat/refresh_rate.cpp", "src/quest/combat/pfx.cpp", "src/quest/combat/headset_gates.cpp"} {
		if asked.MatchString(stripComments(read(t, f))) {
			t.Errorf("%s asks for the gamespaces still loading", f)
		}
	}
	if !asked.MatchString(stripComments(read(t, "src/quest/combat/table_step.cpp"))) {
		t.Error("the table steps' scope does not ask for the gamespaces still loading")
	}
}

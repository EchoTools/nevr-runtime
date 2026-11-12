package questwiring

import (
	"strings"
	"testing"
)

// #2185: gun connect, practice AI bots and the gun hand default are the runtime's. The host tests (gunconnect_test, aibots_test, weapon_arm_test,
// combat_install_test, quest_config_test) drive the behaviour; these pin what they cannot see: that the three install no hook and write no page (so the
// hook budget has no row and N151 holds), where they sit in the table, that their keys are read, known, logged and documented and default on, that the
// plugin's twins and its option are gone, and that the plugin's marker directory is the one the module uses (player state, ADR 0046 rule 4).

var gunBotsModules = []string{"gunconnect", "aibots", "weapon_arm"}

func TestTheThreeModulesHookNothingAndWriteNoPage(t *testing.T) {
	budget := read(t, "src/quest/sentinel/hook_budget.h")
	for _, m := range gunBotsModules {
		for _, f := range []string{m + ".cpp", m + ".h", m + "_targets.h"} {
			code := stripComments(read(t, "src/quest/combat/"+f))
			for _, bad := range []string{"callback_thunk", "InstallThunk", "InstallAsmEntry", "GotHook", "GotDataSlot", "mprotect", "mmap", "SwapWord", "PROT_WRITE", "PROT_EXEC"} {
				if strings.Contains(code, bad) {
					t.Errorf("%s names %s: the module calls the game and installs nothing", f, bad)
				}
			}
		}
		if strings.Contains(budget, "combat/"+m+".cpp") {
			t.Errorf("hook_budget.h has a row for %s, which declares no hook handle", m)
		}
		// the tick callback is the only registration
		if !strings.Contains(stripComments(read(t, "src/quest/combat/"+m+".cpp")), "tick::Add(&Tick)") {
			t.Errorf("%s.cpp does not register its tick callback", m)
		}
	}
}

func TestTheRowsFollowTheTickAndPrecedeTheGatesAndReadTheirOwnKeys(t *testing.T) {
	install := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	rr := strings.Index(install, `Module{"refresh_rate"`)
	gc := strings.Index(install, `Module{"gunconnect", nullptr, &GunconnectNormal, "combat.gunconnect"`)
	ai := strings.Index(install, `Module{"aibots", nullptr, &AibotsNormal, "combat.aibots"`)
	wa := strings.Index(install, `Module{"weapon_arm", nullptr, &WeaponArmNormal, "combat.weapon_arm"`)
	gates := strings.Index(install, `Module{"gates"`)
	if rr < 0 || gc < rr || ai < gc || wa < ai || gates < wa {
		t.Errorf("rows out of the plugin's order or not normal-only: refresh_rate=%d gunconnect=%d aibots=%d weapon_arm=%d gates=%d", rr, gc, ai, wa, gates)
	}
	for _, want := range []string{"gunconnect::Install(g_config.gunconnect.on)", "switches.on = g_config.aibots.on", "g_config.aibotsScenario", "switches.devBuild = DevBuild()", "weapon_arm::Install(g_config.weaponArm.on)"} {
		if !strings.Contains(install, want) {
			t.Errorf("combat_install.cpp does not contain %s", want)
		}
	}
}

func TestEachKeyIsReadKnownDocumentedAndDefaultsOn(t *testing.T) {
	reader, schema := read(t, "src/quest/sentinel/quest_config.cpp"), read(t, "src/core/nevr_config.cpp")
	example, header := read(t, "docs/reference/example-config.yaml"), read(t, "src/quest/sentinel/quest_config.h")
	for _, key := range []string{"gunconnect", "aibots", "aibots_scenario", "weapon_arm"} {
		if !strings.Contains(reader, `"combat.`+key+`"`) {
			t.Errorf("quest_config.cpp does not read combat.%s", key)
		}
		if !strings.Contains(schema, `"`+key+`"`) {
			t.Errorf("the schema does not know combat.%s", key)
		}
		if !strings.Contains(example, "\n  # "+key+": ") {
			t.Errorf("the example config does not document %s", key)
		}
	}
	// the three switches default on (upstream's shipped build has all three on): no fallback argument other than the default, no `{false` initialiser
	for _, call := range []string{"ReadCombatSwitch(cfg, kCombatGunconnectKey, r)", "ReadCombatSwitch(cfg, kCombatAibotsKey, r)", "ReadCombatSwitch(cfg, kCombatWeaponArmKey, r)"} {
		if !strings.Contains(reader, call) {
			t.Errorf("quest_config.cpp: %s is not read with the default fallback", call)
		}
	}
	for _, member := range []string{"CombatSwitch gunconnect;", "CombatSwitch aibots;", "CombatSwitch weaponArm;"} {
		if !strings.Contains(header, member) {
			t.Errorf("quest_config.h: %s is not a default-on switch", member)
		}
	}
}

func TestEachModuleHasItsDocMutantsAndTest(t *testing.T) {
	for _, n := range gunBotsModules {
		for _, p := range []string{"docs/quest/combat/" + n + ".md", "src/quest/combat/mutants.d/" + n + ".sh", "src/quest/tests/" + n + "_test.cpp"} {
			if !exists(t, p) {
				t.Errorf("%s does not exist", p)
			}
		}
	}
	if !exists(t, "docs/quest/combat/adr/0003-weapon-arm-new-profile.md") {
		t.Error("the weapon-arm decision did not move to docs/quest/combat/adr")
	}
}

func TestTheMarkerDirectoryStaysWherePlayersProfilesAre(t *testing.T) {
	legacy := read(t, "src/quest/combat/legacy_switch.h")
	if !strings.Contains(legacy, `inline constexpr const char* kDataSubdir = "q15t";`) ||
		!strings.Contains(legacy, `"/sdcard/Android/data/%.160s/files/%s", pkg, kDataSubdir`) {
		t.Error("legacy_switch.h does not name the data directory players' gun-hand markers are in")
	}
	for _, f := range []string{"src/quest/combat/table_step.cpp", "src/quest/combat/sendguard.cpp"} {
		if code := stripComments(read(t, f)); strings.Contains(code, "/files/") || !strings.Contains(code, "legacy::") {
			t.Errorf("%s names the data directory itself: it takes it from legacy_switch.h", f)
		}
	}
	arm := read(t, "src/quest/combat/weapon_arm.cpp")
	if !strings.Contains(arm, `"%s/t26arm.%s"`) || !strings.Contains(arm, "legacy::Directory(out, n)") {
		t.Error("weapon_arm.cpp does not name its marker as the plugin did, in the plugin's directory")
	}
}

// #2382: practice AI bots decide on the combat level answer's last look (combat_scope.h LastLook), not on a scan of the loaded levels at
// each call, and their install is refused when that answer cannot change. The host tests drive the behaviour; these pin the wiring.
func TestAibotsReadsOneLookOfTheScopeScansNoLevelAndRefusesWhenTheScopeCannotChange(t *testing.T) {
	t.Parallel()
	code := stripComments(read(t, "src/quest/combat/aibots.cpp"))
	if strings.Contains(code, "combat_map.h") {
		t.Error("aibots.cpp reads the plugin's combat_map copy")
	}
	if strings.Contains(code, "combat_level.h") {
		t.Error("aibots.cpp includes combat_level.h: its decision reads the scope's last look, not the levels")
	}
	for _, scan := range []string{"level::Scan(", "level::Classify("} {
		if strings.Contains(code, scan) {
			t.Errorf("aibots.cpp calls %s: the decision reads scope::LastLook() once", scan)
		}
	}
	if n := strings.Count(code, "scope::LastLook()"); n != 1 {
		t.Errorf("aibots.cpp calls scope::LastLook() %d times, want once per decision", n)
	}
	// The install asks the scope before it looks up the library or hooks anything, and refuses when the answer cannot change (as the
	// wrist HUD and shield gamma modules do); an off module asks nothing of the scope; the module does not register the refresh itself.
	install := body(t, code, "ModuleResult InstallAt(")
	before(t, install, "off_config", "scope::Live()")
	before(t, install, "scope::Live()", "lookup(sentinel::pinned::kLibR15")
	before(t, install, "scope_not_live", "lookup(sentinel::pinned::kLibR15")
	if !strings.Contains(install, `Refused("scope_not_live"`) {
		t.Error(`aibots.cpp's install does not refuse with "scope_not_live"`)
	}
	// The refresh is registered once, by the scope row: combat_install.cpp's ScopeNormal returns scope::InstallNormal(), which adds scope::Refresh
	// to the tick. The module names neither, and the row's call is where this test expects it, so a rename cannot leave the check reading nothing.
	for _, reg := range []string{"scope::Refresh", "scope::InstallNormal("} {
		if strings.Contains(code, reg) {
			t.Errorf("aibots.cpp names %s: it registers the refresh itself; the scope row does", reg)
		}
	}
	if !strings.Contains(stripComments(read(t, "src/quest/combat/combat_install.cpp")), "return scope::InstallNormal();") {
		t.Error("combat_install.cpp's scope row does not call scope::InstallNormal(): the registration this test keeps out of aibots.cpp is not where it expects it")
	}
}

package questwiring

import (
	"strings"
	"testing"
)

// #2185: gun connect, practice AI bots and the new-profile gun hand are the runtime's. The host tests (gunconnect_test, aibots_test, weapon_arm_test,
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

func TestThePluginHoldsNoneOfThemAndTheDocsMoved(t *testing.T) {
	for _, n := range gunBotsModules {
		for _, p := range []string{
			"src/quest/plugins/q15t/src/" + n + ".cpp", "src/quest/plugins/q15t/src/" + n + ".h",
			"src/quest/tests/q15t/mod/" + n + ".cmake", "src/quest/tests/q15t/mutants.d/" + n + ".sh", "docs/quest/q15t/modules/" + n + ".md",
		} {
			if exists(t, p) {
				t.Errorf("%s still exists", p)
			}
		}
		for _, p := range []string{"docs/quest/combat/" + n + ".md", "src/quest/combat/mutants.d/" + n + ".sh", "src/quest/tests/" + n + "_test.cpp"} {
			if !exists(t, p) {
				t.Errorf("%s does not exist", p)
			}
		}
	}
	for _, p := range []string{"src/quest/tests/q15t/gunconnect_test.cpp", "src/quest/tests/q15t/aibots_test.cpp", "src/quest/tests/q15t/t26_test.cpp", "docs/quest/q15t/adr/0002-weapon-arm-new-profile.md"} {
		if exists(t, p) {
			t.Errorf("%s still exists", p)
		}
	}
	if !exists(t, "docs/quest/combat/adr/0003-weapon-arm-new-profile.md") {
		t.Error("the weapon-arm decision did not move to docs/quest/combat/adr")
	}
	plugin := read(t, "src/quest/plugins/q15t/CMakeLists.txt") + read(t, "src/quest/plugins/q15t/src/plugin.cpp")
	for _, bad := range []string{"Q15T_T26", "Q15T_GUNCONNECT", "Q15T_AIBOTS", "GunConnectTick", "AiBotsTick", "WeaponArmTick"} {
		if strings.Contains(plugin, bad) {
			t.Errorf("the plugin still names %s", bad)
		}
	}
}

func TestTheMarkerDirectoryStaysWherePlayersProfilesAre(t *testing.T) {
	legacy := read(t, "src/quest/combat/legacy_switch.h")
	if !strings.Contains(legacy, `/sdcard/Android/data/%.160s/files/q15t`) {
		t.Error("the directory the combat modules read the plugin's files from is not files/q15t")
	}
	arm := read(t, "src/quest/combat/weapon_arm.cpp")
	if !strings.Contains(arm, `"%s/t26arm.%s"`) || !strings.Contains(arm, "legacy::Directory(out, n)") {
		t.Error("weapon_arm.cpp does not name its marker as the plugin did, in the plugin's directory")
	}
}

func TestTheCombatMapDetectorIsNoLongerReadByAibots(t *testing.T) {
	if strings.Contains(read(t, "src/quest/combat/aibots.cpp"), "combat_map.h") {
		t.Error("aibots.cpp reads the plugin's combat_map copy: it asks combat_level")
	}
	if !strings.Contains(read(t, "src/quest/combat/aibots.cpp"), "level::Scan(game)") {
		t.Error("aibots.cpp does not ask combat_level for the loaded levels")
	}
}

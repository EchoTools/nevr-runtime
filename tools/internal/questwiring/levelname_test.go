package questwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2181: levelname and the booster pool are the runtime's, and the q15t plugin carries neither. levelname_test, boosterpool_test and
// combat_install_test drive the behaviour; these pin what they cannot see (the order of a handler's calls, the table, the files that are gone).

func TestTheTwoModulesAreNormalRowsAfterTheRefreshRateAndTheBoosterNeedsLevelname(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	before(t, table, `Module{"refresh_rate", nullptr, &RefreshRateNormal}`, `Module{"levelname", nullptr, &LevelNameNormal}`)
	before(t, table, `Module{"levelname", nullptr, &LevelNameNormal}`, `Module{"boosterpool", nullptr, &BoosterPoolNormal}`)
	rows := body(t, table, "ModuleResult BoosterPoolNormal()")
	if !strings.Contains(rows, "levelname::Installed()") || !strings.Contains(rows, "g_config.levelname.on") {
		t.Errorf("the booster pool's row does not ask for levelname's install and the key: %s", rows)
	}
	if !strings.Contains(body(t, table, "ModuleResult LevelNameNormal()"), "g_config.levelname.on") {
		t.Error("levelname's row does not read its key")
	}
}

func TestTheHandlersLogAfterTheOriginalAnswersNeverBefore(t *testing.T) {
	ln := stripComments(read(t, "src/quest/combat/levelname.cpp"))
	answer := body(t, ln, "std::uint64_t Answer(")
	before(t, answer, "plain(game, name)", "NoteKept(name)")
	bp := stripComments(read(t, "src/quest/combat/boosterpool.cpp"))
	handle := body(t, bp, "ActorPair Handle(")
	before(t, handle, "original(st, plain)", "NoteKept(name)")
	for name, code := range map[string]string{"levelname": answer, "boosterpool": handle} {
		if strings.Contains(code, "LogFields") {
			t.Errorf("the %s handler writes a line itself: it logs through NoteKept, after the original returned", name)
		}
	}
}

func TestTheKeyIsReadKnownLoggedAndDocumentedAndDefaultsOn(t *testing.T) {
	reader, schema := read(t, "src/quest/sentinel/quest_config.cpp"), read(t, "src/core/nevr_config.cpp")
	line, example := read(t, "src/quest/combat/combat_install.cpp"), read(t, "docs/reference/example-config.yaml")
	if !strings.Contains(reader, `"combat.levelname"`) || !strings.Contains(reader, "ReadCombatSwitch(cfg, kCombatLevelNameKey, r)") {
		t.Error("the reader does not read combat.levelname with the default fallback (on)")
	}
	if !strings.Contains(schema, `"levelname"}}`) {
		t.Error("the schema's combat section does not name levelname")
	}
	if !strings.Contains(line, `"levelname_source"`) {
		t.Error("the combat_config line does not name levelname")
	}
	if !strings.Contains(example, "\n  # levelname: ") {
		t.Error("the example config does not document combat.levelname")
	}
	if !regexp.MustCompile(`CombatSwitch levelname\{true, Source::kAbsent\}`).MatchString(read(t, "src/quest/sentinel/quest_config.h")) {
		t.Error("combat.levelname does not default to on")
	}
}

func TestThePluginCarriesNeitherModuleAndItsMarkerLostTheField(t *testing.T) {
	for _, n := range []string{"levelname", "boosterpool"} {
		for _, e := range []string{"h", "cpp"} {
			if exists(t, "src/quest/plugins/q15t/src/"+n+"."+e) {
				t.Errorf("the plugin still has %s.%s", n, e)
			}
		}
		for _, f := range []string{"src/quest/tests/q15t/" + n + "_test.cpp", "src/quest/tests/q15t/mod/" + n + ".cmake", "src/quest/tests/q15t/mutants.d/" + n + ".sh"} {
			if exists(t, f) {
				t.Errorf("%s still exists", f)
			}
		}
		for _, f := range []string{"src/quest/combat/" + n + ".cpp", "src/quest/combat/mutants.d/" + n + ".sh", "src/quest/tests/" + n + "_test.cpp"} {
			if !exists(t, f) {
				t.Errorf("%s is missing", f)
			}
		}
	}
	plugin := stripComments(read(t, "src/quest/plugins/q15t/src/plugin.cpp"))
	for _, bad := range []string{"levelname.h", "boosterpool.h", "Q15T_LEVELNAME", "InstallLevelName", "InstallBoosterPool"} {
		if strings.Contains(plugin, bad) {
			t.Errorf("plugin.cpp still names %s", bad)
		}
	}
	for _, f := range []string{"src/quest/plugins/q15t/CMakeLists.txt", "justfile", "src/quest/tests/q15t/mod/wiring.cmake"} {
		code := read(t, f)
		if strings.Contains(code, "Q15T_LEVELNAME") || strings.Contains(code, " levelname=1") {
			t.Errorf("%s still names the deleted option or its marker field", f)
		}
	}
	if !exists(t, "docs/quest/combat/levelname.md") || exists(t, "docs/quest/q15t/modules/levelname.md") {
		t.Error("the module doc is not where the combat docs live")
	}
	// the bot-body gate still depends on the full level, which the runtime loads: the plugin takes it as met and ignores the kill file
	rawPlugin := read(t, "src/quest/plugins/q15t/src/plugin.cpp")
	if !strings.Contains(plugin, "gateOff.levelname = false") || !strings.Contains(rawPlugin, "/*levelname=*/true") {
		t.Error("the plugin's bot-body gate does not take levelname as met (KNOWN-WRONG #2180)")
	}
}

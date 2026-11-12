package questwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2181: levelname and the booster pool are the runtime's. levelname_test, boosterpool_test and combat_install_test drive the behaviour; these
// pin what they cannot see (the order of a handler's calls, the table, each module's files, the release recipes' marker).

func TestTheTwoModulesAreNormalRowsAfterTheRefreshRateAndTheBoosterNeedsLevelname(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	before(t, table, `Module{"refresh_rate", nullptr, &RefreshRateNormal}`, `Module{"levelname", nullptr, &LevelNameNormal, "combat.levelname", &nevr_quest::CombatConfig::levelname, &levelname::NoteEnd}`)
	before(t, table, `Module{"levelname", nullptr, &LevelNameNormal, "combat.levelname", &nevr_quest::CombatConfig::levelname, &levelname::NoteEnd}`, `Module{"boosterpool", nullptr, &BoosterPoolNormal, nullptr, nullptr, &boosterpool::NoteEnd}`)
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
	if !strings.Contains(schema, `"refresh_rate_quest2_72", "levelname", `) {
		t.Error("the schema's combat section does not name levelname")
	}
	if !strings.Contains(line, `"combat_config_switch"`) || strings.Contains(line, `"levelname_source"`) {
		t.Error("levelname's switch is not reported by the generated combat_config_switch record (and must not be a field of the combat_config line)")
	}
	if !strings.Contains(example, "\n  # levelname: ") {
		t.Error("the example config does not document combat.levelname")
	}
	if !regexp.MustCompile(`CombatSwitch levelname\{true, Source::kAbsent\}`).MatchString(read(t, "src/quest/sentinel/quest_config.h")) {
		t.Error("combat.levelname does not default to on")
	}
}

func TestEachModuleHasItsFilesAndNoRecipeWritesTheOldMarkerField(t *testing.T) {
	for _, n := range []string{"levelname", "boosterpool"} {
		for _, f := range []string{"src/quest/combat/" + n + ".cpp", "src/quest/combat/mutants.d/" + n + ".sh", "src/quest/tests/" + n + "_test.cpp"} {
			if !exists(t, f) {
				t.Errorf("%s is missing", f)
			}
		}
	}
	if strings.Contains(read(t, "justfile"), " levelname=1") {
		t.Error("the justfile still writes the deleted marker field")
	}
	if !exists(t, "docs/quest/combat/levelname.md") {
		t.Error("the module doc is not where the combat docs live")
	}
}

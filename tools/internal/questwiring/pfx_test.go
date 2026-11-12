package questwiring

import (
	"strings"
	"testing"
)

// #2188, #1631, #1642: the particle-renderer gate and the event-effect decision are the runtime's. The host tests (pfx_test, pfxtev_test,
// combat_install_test, quest_config_test) drive the behaviour; these pin what they cannot see: that the modules hook nothing and register no
// tick (so arena pays nothing at install or on a frame), where they sit in the table, that their keys are read, known, logged and documented, and
// that the plugin's twins are gone.

func TestThePfxModulesHookNothingAndRegisterNoTick(t *testing.T) {
	for _, f := range []string{"pfx.cpp", "pfx.h", "pfxtev.cpp", "pfxtev.h"} {
		code := stripComments(read(t, "src/quest/combat/"+f))
		for _, bad := range []string{"callback_thunk", "InstallThunk", "InstallAsmEntry", "GotHook", "GotDataSlot", "tick::Add", "combat_tick.h", "mprotect", "SwapWord"} {
			if strings.Contains(code, bad) {
				t.Errorf("%s names %s: the module is a decision and must install nothing", f, bad)
			}
		}
	}
	// Which levels the gate is shut on (the combat lobby and the lobbies arena players use included) is pfx_test's, driven through a fake game.
	if strings.Contains(read(t, "src/quest/sentinel/hook_budget.h"), "pfx") {
		t.Error("hook_budget.h has a row for a module that installs no hook")
	}
}

func TestThePfxRowsAreNormalOnlyAfterTheRefreshRateRowAndReadTheirOwnKeys(t *testing.T) {
	install := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	rr := strings.Index(install, `Module{"refresh_rate"`)
	p := strings.Index(install, `Module{"pfx", nullptr, &PfxNormal}`)
	tev := strings.Index(install, `Module{"pfxtev", nullptr, &PfxtevNormal}`)
	if rr < 0 || p < rr || tev < p {
		t.Errorf("the rows are out of order or not normal-only: refresh_rate=%d pfx=%d pfxtev=%d", rr, p, tev)
	}
	for _, want := range []string{"pfx::Install(g_config.pfx.on)", "pfxtev::Install(g_config.pfxtev.on)"} {
		if !strings.Contains(install, want) {
			t.Errorf("combat_install.cpp does not call %s", want)
		}
	}
}

func TestEachPfxKeyIsReadKnownLoggedAndDocumentedAndDefaultsOn(t *testing.T) {
	reader, schema := read(t, "src/quest/sentinel/quest_config.cpp"), read(t, "src/core/nevr_config.cpp")
	line, example := read(t, "src/quest/combat/combat_install.cpp"), read(t, "docs/reference/example-config.yaml")
	for _, key := range []string{"pfx", "pfxtev"} {
		for what, text := range map[string]string{"reader": reader, "schema": schema, "line": line} {
			if !strings.Contains(text, `"`+key+`"`) && !strings.Contains(text, `"combat.`+key+`"`) {
				t.Errorf("%s does not name %s", what, key)
			}
		}
		if !strings.Contains(example, "\n  # "+key+": ") {
			t.Errorf("the example config does not document %s", key)
		}
	}
	if !strings.Contains(reader, "ReadCombatSwitch(cfg, kCombatPfxKey, r)") || !strings.Contains(reader, "ReadCombatSwitch(cfg, kCombatPfxtevKey, r)") {
		t.Error("a pfx key is read with a fallback other than on")
	}
}

func TestEachPfxModuleHasItsDoc(t *testing.T) {
	for _, n := range []string{"pfx", "pfxtev"} {
		if !exists(t, "docs/quest/combat/"+n+".md") {
			t.Errorf("docs/quest/combat/%s.md is missing", n)
		}
	}
}

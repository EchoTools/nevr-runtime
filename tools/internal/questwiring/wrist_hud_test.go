package questwiring

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// #2186: the wrist HUD heat meter and the shield colour are the runtime's, and the q15t plugin installs on neither slot. The host tests
// (hudmeter_test, shieldgamma_test, combat_scope_test) drive the behaviour; these pin what they cannot see: the order inside a handler and an
// install function, the registration table's order, and what the plugin no longer holds.

func TestEachHandlerIsTheTestAndATailCallAndTheBodyChecksTheLookIsFresh(t *testing.T) {
	for _, h := range []struct{ file, sig, body string }{
		{"src/quest/combat/hudmeter.cpp", "std::uint64_t OnEnter(", "std::uint64_t ActInCombat("},
		{"src/quest/combat/shieldgamma.cpp", "std::uint64_t OnUpdate(", "std::uint64_t ActInCombat("},
	} {
		code := stripComments(read(t, h.file))
		handler := body(t, code, h.sig)
		// the handler is the arena path: the test, then the game's function with the very same arguments, or the combat body
		inner := strings.Join(strings.Fields(handler[strings.Index(handler, "{")+1:strings.LastIndex(handler, "}")]), " ")
		want := "if (!scope::SeenCombatOrLobby()) return original(node, data); return ActInCombat(original, node, data);"
		if inner != want {
			t.Errorf("%s: the handler is not just the scope test and the two tail calls: %q", h.file, inner)
		}
		// the combat body is a function of its own that no compiler folds back into the handler, and starts with the staleness test
		if !strings.Contains(read(t, h.file), "[[gnu::noinline]] "+h.body) {
			t.Errorf("%s: the combat body is not [[gnu::noinline]]", h.file)
		}
		b := body(t, code, h.body)
		first := strings.TrimSpace(b[strings.Index(b, "{")+1:])
		if !strings.HasPrefix(first, "if (!scope::LookIsFresh()) return original(node, data);") {
			t.Errorf("%s: the combat body does not start with the staleness test", h.file)
		}
		for _, later := range []string{"SafeRead(", "g_unreadable", "fetch_add", "memcpy"} {
			before(t, b, "scope::LookIsFresh()", later)
		}
		if strings.Contains(handler, "SafeRead(") || strings.Contains(handler, "LookIsFresh") {
			t.Errorf("%s: the handler reads the game or the clock", h.file)
		}
	}
}

func TestEachModuleAsksTheScopeIsLiveBeforeItHooksAnything(t *testing.T) {
	for _, m := range []struct{ file, sig string }{
		{"src/quest/combat/hudmeter.cpp", "ModuleResult InstallAt("},
		{"src/quest/combat/shieldgamma.cpp", "ModuleResult InstallAt("},
	} {
		code := stripComments(read(t, m.file))
		b := body(t, code, m.sig)
		before(t, b, "scope::Live()", "sentinel::InstallThunk<Thunk>(")
		before(t, b, "scope_not_live", "sentinel::InstallThunk<Thunk>(")
		if !strings.Contains(b, "off_config") {
			t.Errorf("%s: an off module does not say off_config", m.file)
		}
		before(t, b, "off_config", "scope::Live()") // a module that is off asks nothing of the scope
		if strings.Contains(code, "scope::Enroll") {
			t.Errorf("%s: a module registers the refresh itself; the scope row does", m.file)
		}
	}
}

func TestTheScopeRefreshIsTheTicksCallbackRegisteredByItsOwnRowAndLooksAtTheGameSeldom(t *testing.T) {
	code := stripComments(read(t, "src/quest/combat/combat_scope.cpp"))
	if !strings.Contains(code, "tick::Add(&Refresh)") {
		t.Error("the level answer is not refreshed from the combat tick")
	}
	b := body(t, code, "void Refresh(")
	before(t, b, "NowMs()", "level::Scan(")
	before(t, b, "g_nextLookMs", "level::Scan(")
	before(t, b, "g_lookedAtMs.store(now", "g_bits.store(bits")
	in := body(t, code, "ModuleResult InstallNormal(")
	before(t, in, "tick::Installed()", "tick::Add(&Refresh)")
	if strings.Contains(stripComments(read(t, "src/quest/combat/combat_scope.h")), "SafeRead") {
		t.Error("the cache's header reads the game")
	}
	header := stripComments(read(t, "src/quest/combat/combat_scope.h"))
	if !strings.Contains(header, "inline std::atomic<std::uint32_t> g_bits{0};") {
		t.Error("the answer is not an inline atomic word in the header")
	}
}

func TestTheRowsInstallAfterTheRefreshRateInThePluginsOrder(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	before(t, table, `Module{"tick", nullptr, &tick::InstallNormal}`, `Module{"scope", nullptr, &scope::InstallNormal}`)
	before(t, table, `Module{"scope", nullptr, &scope::InstallNormal}`, `Module{"refresh_rate", nullptr, &RefreshRateNormal}`)
	before(t, table, `Module{"refresh_rate", nullptr, &RefreshRateNormal}`, `Module{"hudmeter", nullptr, &HudmeterNormal`)
	before(t, table, `Module{"hudmeter", nullptr, &HudmeterNormal`, `Module{"shieldgamma", nullptr, &ShieldgammaNormal}`)
	// the three switches are generated records of their own (`combat_config_switch`), each read from its member
	for _, row := range []string{
		`Module{"hudmeter", nullptr, &HudmeterNormal, "combat.hudmeter", &nevr_quest::CombatConfig::hudmeter}`,
		`Module{"shieldgamma_gamma", nullptr, nullptr, "combat.shieldgamma_gamma", &nevr_quest::CombatConfig::shieldgammaGamma}`,
		`Module{"shieldgamma_tube_mirror", nullptr, nullptr, "combat.shieldgamma_tube_mirror", &nevr_quest::CombatConfig::shieldgammaTubeMirror}`,
	} {
		if !strings.Contains(table, row) {
			t.Errorf("the table has no row %s", row)
		}
	}
	for _, want := range []string{
		"hudmeter::Install(g_config.hudmeter.on)",
		"shieldgamma::Install(g_config.shieldgammaGamma.on, g_config.shieldgammaTubeMirror.on)",
	} {
		if !strings.Contains(table, want) {
			t.Errorf("the table's rows do not read the module's switches: no %q", want)
		}
	}
	budget := read(t, "src/quest/sentinel/hook_budget.h")
	for _, row := range []string{`{"combat/hudmeter.cpp", 1, 0}`, `{"combat/shieldgamma.cpp", 1, 0}`} {
		if !strings.Contains(budget, row) {
			t.Errorf("no hook budget row %s", row)
		}
	}
	for _, p := range []string{"hudmeter_targets.h", "shieldgamma_targets.h"} {
		targets := stripComments(read(t, "src/quest/combat/"+p))
		if !strings.Contains(targets, "target.expectedOriginal = reinterpret_cast<const void*>(base + kFnVaddr)") || !strings.Contains(targets, ".Owned(\"combat_") {
			t.Errorf("%s: the target does not pin the game's function as the slot's original and name its owner", p)
		}
	}
}

func TestEachNewKeyIsReadKnownLoggedAndDocumentedAndDefaultsAsTheModuleNeeds(t *testing.T) {
	reader, schema := read(t, "src/quest/sentinel/quest_config.cpp"), read(t, "src/core/nevr_config.cpp")
	line, example := read(t, "src/quest/combat/combat_install.cpp"), read(t, "docs/reference/example-config.yaml")
	for _, key := range []string{"hudmeter", "shieldgamma_gamma", "shieldgamma_tube_mirror"} {
		for what, text := range map[string]string{"reader": reader, "schema": schema, "line": line} {
			if !strings.Contains(text, `"`+key+`"`) && !strings.Contains(text, `"combat.`+key+`"`) {
				t.Errorf("%s does not name %s", what, key)
			}
		}
		if !strings.Contains(example, "\n  # "+key+": ") {
			t.Errorf("the example config does not document %s", key)
		}
	}
	header := read(t, "src/quest/sentinel/quest_config.h")
	// the gamma part was a test switch in the reference build: it is the one that defaults off, in the struct and in the reader
	if !strings.Contains(header, "CombatSwitch shieldgammaGamma{false, Source::kAbsent}") || !strings.Contains(header, "CombatSwitch hudmeter{true, Source::kAbsent}") ||
		!strings.Contains(header, "CombatSwitch shieldgammaTubeMirror{true, Source::kAbsent}") {
		t.Error("a key's default in CombatConfig is not what its module needs")
	}
	if !strings.Contains(reader, "ReadCombatSwitch(cfg, kCombatShieldGammaKey, r, /*fallback=*/false)") {
		t.Error("the reader's fallback for combat.shieldgamma_gamma is not off")
	}
}

func TestThePluginHoldsNeitherSlotAndNoLongerBuildsEitherModule(t *testing.T) {
	for _, n := range []string{"hudmeter", "shieldgamma"} {
		for _, e := range []string{"h", "cpp"} {
			if exists(t, "src/quest/plugins/q15t/src/"+n+"."+e) {
				t.Errorf("the plugin still has %s.%s", n, e)
			}
		}
		for _, p := range []string{"src/quest/tests/q15t/" + n + "_test.cpp", "src/quest/tests/q15t/mod/" + n + ".cmake", "src/quest/tests/q15t/mutants.d/" + n + ".sh", "docs/quest/q15t/modules/" + n + ".md"} {
			if exists(t, p) {
				t.Errorf("%s is still the plugin's", p)
			}
		}
		for _, p := range []string{"docs/quest/combat/" + n + ".md", "src/quest/combat/mutants.d/" + n + ".sh", "src/quest/tests/" + n + "_test.cpp"} {
			if !exists(t, p) {
				t.Errorf("%s is missing", p)
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
		for _, bad := range []string{"InstallHudmeter", "InstallShieldGamma", "Q15T_HUDMETER", "Q15T_SHIELDGAMMA", "0x36c6ce8", "0x36d2020"} {
			if strings.Contains(code, bad) {
				t.Errorf("%s names %s", e.Name(), bad)
			}
		}
	}
	cmake := read(t, "src/quest/plugins/q15t/CMakeLists.txt") + read(t, "src/quest/tests/q15t/mod/wiring.cmake")
	for _, bad := range []string{"Q15T_HUDMETER", "Q15T_SHIELDGAMMA", "src/hudmeter", "src/shieldgamma", " hudmeter ", " shieldgamma ", "(HUDMETER", "SHIELDGAMMA="} {
		if strings.Contains(cmake, bad) {
			t.Errorf("the plugin's CMake or its wiring test still names %s", bad)
		}
	}
}

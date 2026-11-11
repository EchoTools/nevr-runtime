package questwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #2172, #2000: the refresh-rate rule and the game tick are the runtime's, and the q15t plugin installs on neither slot. The host tests
// (refresh_rate_test, combat_tick_test, combat_level_test, render_config_hook_test) drive the behaviour; these pin what they cannot see.

func root(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	r := filepath.Join(wd, "..", "..", "..")
	if _, err := os.Stat(filepath.Join(r, "tools", "go.mod")); err != nil {
		t.Fatalf("%s is not the repository root: %v", r, err)
	}
	return r
}

func read(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(root(t), path))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func exists(t *testing.T, path string) bool {
	t.Helper()
	_, err := os.Stat(filepath.Join(root(t), path))
	return err == nil
}

// The q15t plugin and its host tests are deleted (#2189). A check that the plugin holds none of a module's code reads the plugin through
// these: a deleted plugin holds nothing, so they answer empty; while it exists they read it as before, and a missing file is a failure.
const pluginDir = "src/quest/plugins/q15t"

func pluginGone(t *testing.T) bool {
	t.Helper()
	return !exists(t, pluginDir) && !exists(t, "src/quest/tests/q15t")
}

func pluginRead(t *testing.T, path string) string {
	t.Helper()
	if pluginGone(t) {
		return ""
	}
	return read(t, path)
}

func pluginEntries(t *testing.T, dir string) []os.DirEntry {
	t.Helper()
	if pluginGone(t) {
		return nil
	}
	entries, err := os.ReadDir(filepath.Join(root(t), dir))
	if err != nil {
		t.Fatal(err)
	}
	return entries
}

func TestTheQ15tPluginAndItsTestsAreGone(t *testing.T) {
	for _, p := range []string{pluginDir, "src/quest/tests/q15t", "docs/quest/q15t"} {
		if exists(t, p) {
			t.Errorf("%s exists: the q15t plugin is folded into the runtime (#2189)", p)
		}
	}
}

var blockComment = regexp.MustCompile(`(?s)/\*.*?\*/`)
var lineComment = regexp.MustCompile(`//[^\n]*`)

func stripComments(s string) string {
	return lineComment.ReplaceAllString(blockComment.ReplaceAllString(s, ""), "")
}

// body returns the text of the braced function whose signature starts with sig.
func body(t *testing.T, code, sig string) string {
	t.Helper()
	at := strings.Index(code, sig)
	if at < 0 {
		t.Fatalf("no %q", sig)
	}
	open := strings.Index(code[at:], "{")
	depth := 0
	for i := at + open; i < len(code); i++ {
		switch code[i] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return code[at : i+1]
			}
		}
	}
	t.Fatalf("unbalanced %q", sig)
	return ""
}

func before(t *testing.T, s, a, b string) {
	t.Helper()
	ia, ib := strings.Index(s, a), strings.Index(s, b)
	if ia < 0 || ib < 0 || ia >= ib {
		t.Errorf("want %q before %q (at %d and %d)", a, b, ia, ib)
	}
}

func TestTheRenderConfigHandlerAsksTheOwnerBeforeTheOriginalAndReportsAfter(t *testing.T) {
	b := body(t, stripComments(read(t, "src/quest/sentinel/render_config_hook.cpp")), "int OnSetDisplayRefreshRate(")
	before(t, b, "combat::refresh_rate::Choose(ovr, rate)", "original(ovr, sent)")
	before(t, b, "original(ovr, sent)", "combat::refresh_rate::Note(rate, sent, result)")
	if strings.Contains(b, "original(ovr, rate)") || strings.Contains(b, "ToMilli(rate)") {
		t.Error("the handler calls the original or reports with the game's own rate instead of the rule's answer")
	}
}

func TestTheTickInstallsBeforeTheRuleInTheNormalPhase(t *testing.T) {
	table := stripComments(read(t, "src/quest/combat/combat_install.cpp"))
	before(t, table, `Module{"tick", nullptr, &tick::InstallNormal}`, `Module{"refresh_rate", nullptr, &RefreshRateNormal}`)
}

func TestTheTickIsAThunkInstalledThroughTheManagerWithNoExpectedOriginal(t *testing.T) {
	code := stripComments(read(t, "src/quest/combat/combat_tick.cpp"))
	for _, want := range []string{"sentinel::InstallThunk<Thunk>(g_hook, targets::Target(), lookup)", "NEVR_HOOK_RECORD(kTickHook, Thunk, &OnTick);"} {
		if !strings.Contains(code, want) {
			t.Errorf("combat_tick.cpp lacks %q", want)
		}
	}
	before(t, body(t, code, "void OnTick("), "original(game);", "RunCallbacks(object)")
	targets := stripComments(read(t, "src/quest/combat/combat_tick_targets.h"))
	if regexp.MustCompile(`GotTarget\{[^}]*kStockVaddr`).MatchString(targets) {
		t.Error("the tick pins the stock function: the plugin's wrapper in the slot would be refused")
	}
	if !strings.Contains(targets, `.Owned("combat_tick")`) {
		t.Error("the tick target names no owner")
	}
	if !strings.Contains(read(t, "src/quest/sentinel/hook_budget.h"), `{"combat/combat_tick.cpp", 1, 0}`) {
		t.Error("no hook budget row for the tick")
	}
	// No plugin wraps the slot any more, so the marker that said one did is gone with it.
	for _, p := range []string{"src/quest/combat/combat_tick.h", "src/quest/combat/combat_tick.cpp"} {
		if strings.Contains(read(t, p), "KNOWN-WRONG #2168") {
			t.Errorf("%s still marks a plugin tick KNOWN-WRONG #2168, and no plugin wraps the slot", p)
		}
	}
}

func TestTheDeviceIsReadOnlyBehindAKnownWrongMarker(t *testing.T) {
	code := read(t, "src/quest/combat/refresh_rate.cpp")
	for _, m := range regexp.MustCompile(`ro\.product\.device`).FindAllStringIndex(code, -1) {
		lo := m[0] - 900
		if lo < 0 {
			lo = 0
		}
		if !strings.Contains(code[lo:m[0]], "KNOWN-WRONG #2037") {
			t.Error("ro.product.device is read without KNOWN-WRONG #2037 above it")
		}
	}
}

func TestTheResolvedSectionReachesTheModulesBeforeTheLineIsWritten(t *testing.T) {
	b := body(t, stripComments(read(t, "src/quest/sentinel/entry.cpp")), "void LogCombatConfig(")
	before(t, b, "combat::SetConfig(config)", "combat::LogConfig(config)")
}

func TestTheLevelAnswerIsSharedNotCopied(t *testing.T) {
	rr := stripComments(read(t, "src/quest/combat/refresh_rate.cpp"))
	if !strings.Contains(rr, "level::Classify(") {
		t.Error("the rule does not ask combat_level.h")
	}
	if regexp.MustCompile(`0x43e2da7914642604`).MatchString(rr) {
		t.Error("the rule carries its own copy of the level list")
	}
}

func TestEachKeyIsReadKnownLoggedAndDocumentedAndDefaultsOn(t *testing.T) {
	reader, schema := read(t, "src/quest/sentinel/quest_config.cpp"), read(t, "src/core/nevr_config.cpp")
	line, example := read(t, "src/quest/combat/combat_install.cpp"), read(t, "docs/reference/example-config.yaml")
	for _, key := range []string{"refresh_rate", "refresh_rate_lobby", "refresh_rate_quest2_72"} {
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
	if !regexp.MustCompile(`struct CombatSwitch \{\n  bool on = true;`).MatchString(header) {
		t.Error("a combat switch does not default to on")
	}
	// arena shares the lobby levels, so the lobby key is the one that defaults off, in the struct and in the reader
	if !strings.Contains(header, "CombatSwitch refreshRateLobby{false, Source::kAbsent}") {
		t.Error("combat.refresh_rate_lobby does not default to off")
	}
	if !strings.Contains(reader, "ReadCombatSwitch(cfg, kCombatRefreshRateLobbyKey, r, /*fallback=*/false)") {
		t.Error("the reader's fallback for combat.refresh_rate_lobby is not off")
	}
}

func TestThePluginInstallsOnNeitherSlotAndTheDeletedModulesAreGone(t *testing.T) {
	for _, n := range []string{"refresh_rate", "clock_levels", "plugin_args"} {
		for _, e := range []string{"h", "cpp"} {
			if exists(t, "src/quest/plugins/q15t/src/"+n+"."+e) {
				t.Errorf("%s.%s still exists", n, e)
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
		for _, bad := range []string{"vrapi_SetClockLevels", "vrapi_SetDisplayRefreshRate", "Q15T_HZ_MATCH90", "SetClockRaise"} {
			if strings.Contains(code, bad) {
				t.Errorf("%s names %s", e.Name(), bad)
			}
		}
	}
	cmake := pluginRead(t, "src/quest/plugins/q15t/CMakeLists.txt")
	for _, bad := range []string{"Q15T_HZ_MATCH90", "Q15T_CPU_LEVEL", "Q15T_GPU_LEVEL", "clock_levels", "refresh_rate", "plugin_args"} {
		if strings.Contains(cmake, bad) {
			t.Errorf("the plugin's CMake still names %s", bad)
		}
	}
	if !pluginGone(t) && !strings.Contains(cmake, "src/combat_map.cpp") {
		t.Error("the detector aibots reads is not built")
	}
	if strings.Contains(pluginRead(t, "src/quest/plugins/q15t/src/plugin.cpp"), "hz_level") || strings.Contains(read(t, "justfile"), "hz_level") {
		t.Error("the config marker still carries hz_level")
	}
	plugin := stripComments(pluginRead(t, "src/quest/plugins/q15t/src/plugin.cpp"))
	for _, bad := range []string{"clock_candidate", "clock_arg", "hz_mode", "ArgString"} {
		if strings.Contains(plugin, bad) {
			t.Errorf("the init line still has %s", bad)
		}
	}
	for _, n := range []string{"0001-match-refresh-rate.md", "0002-level-refresh-rate-default.md"} {
		if !exists(t, "docs/quest/combat/adr/"+n) || exists(t, "docs/quest/q15t/adr/"+n) {
			t.Errorf("%s is not where the combat ADRs live", n)
		}
	}
}

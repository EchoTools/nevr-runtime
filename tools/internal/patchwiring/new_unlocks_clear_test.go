package patchwiring

import (
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"
)

// #2410: the PC new-unlocks clear hooks (src/runtime/patch/new_unlocks_clear.cpp), the parts a host test cannot reach without
// the game. When a clear is held and when the dirty bit is set again is one definition in
// src/runtime/patch/new_unlocks_clear_rules.h, driven by src/runtime/tests/new_unlocks_clear_rules_test.cpp. What stays here is
// the glue's wiring, read as source: the hooked clear and the emptiness test are pinned by bytes at the addresses the rules
// header lists, the hooks go through that table and the rules, the runtime installs them once at boot, and the scenario
// control calls the clear through the detour.

const (
	unlocksGlue  = "src/runtime/patch/new_unlocks_clear.cpp"
	unlocksRules = "src/runtime/patch/new_unlocks_clear_rules.h"
	scenarioCtl  = "src/runtime/scenario/scenario_control.cpp"
)

func TestTheRulesTableNamesThePcFunctions(t *testing.T) {
	t.Parallel()
	text := code(t, unlocksRules)
	mustMatch(t, `kPcAddresses\s*=\s*\{0x1401A06C0ULL,\s*0x1405F1360ULL\}`, text, unlocksRules)
	mustMatch(t, `kQuestAddresses\s*=\s*\{0x1297114ULL,\s*0xFA82F8ULL\}`, text, unlocksRules)
}

func TestTheGluePinsEachFunctionByBytesBeforeHooking(t *testing.T) {
	t.Parallel()
	text := code(t, unlocksGlue)
	for _, name := range []string{"kClearPrologue", "kJsonEmptyPrologue"} {
		mustContain(t, text, name, unlocksGlue)
	}
	for _, va := range []string{"a.clear", "a.jsonEmpty"} {
		mustContain(t, text, "Matches(gameBase, "+va, unlocksGlue)
	}
	before(t, text, "Matches(gameBase, a.clear", "PatchDetour(&g_clearOriginal")
}

func TestThePrologueOfTheClearIsTheOneTheScenarioControlPins(t *testing.T) {
	t.Parallel()
	glue := prologueBytes(t, code(t, unlocksGlue), "kClearPrologue")
	control := prologueBytes(t, code(t, scenarioCtl), "kClearNewUnlocksPrologue")
	if !reflect.DeepEqual(glue, control) {
		t.Errorf("kClearPrologue %v differs from the scenario control's kClearNewUnlocksPrologue %v", glue, control)
	}
}

func TestTheClearIsDetouredAndTheDecisionsAreTheRules(t *testing.T) {
	t.Parallel()
	text := code(t, unlocksGlue)
	mustContain(t, text, `PatchDetour(&g_clearOriginal, reinterpret_cast<void*>(&ClearHook), "ClearNewUnlocks")`, unlocksGlue)
	mustContain(t, text, "g_hold.OnClear(", unlocksGlue)
	mustContain(t, text, "g_hold.OnPoll(", unlocksGlue)
	for _, logic := range []string{"countBefore == 0", "kStillInFlight ="} {
		if strings.Contains(text, logic) {
			t.Errorf("%s holds %q: it belongs to new_unlocks_clear_rules.h", unlocksGlue, logic)
		}
	}
}

func TestTheGameSaveIsNotHooked(t *testing.T) {
	t.Parallel()
	// The live SaveClientProfileChanges is the platform module's, not echovr.exe's: a detour on the echovr.exe copy is never
	// called in a client run.
	text := code(t, unlocksGlue)
	if strings.Contains(text, "SaveClientProfileChanges") {
		t.Errorf("%s names SaveClientProfileChanges", unlocksGlue)
	}
	if n := strings.Count(text, "PatchDetour("); n != 1 {
		t.Errorf("%s calls PatchDetour %d times, want once", unlocksGlue, n)
	}
}

func TestTheOriginalRunsInTheClearHook(t *testing.T) {
	t.Parallel()
	if n := strings.Count(code(t, unlocksGlue), "g_clearOriginal(netGame)"); n != 1 {
		t.Errorf("%s calls g_clearOriginal(netGame) %d times, want once", unlocksGlue, n)
	}
}

func TestTheDirtyBitIsSetWithTheRulesConstantsAfterTheUserIsRechecked(t *testing.T) {
	t.Parallel()
	text := code(t, unlocksGlue)
	mustContain(t, text, "InterlockedOr(reinterpret_cast<volatile LONG*>(Bytes(held) + kUserFlagsOffset), static_cast<LONG>(kUserDirtyBit))", unlocksGlue)
	before(t, text, "reinterpret_cast<void*>(current) != held", "InterlockedOr(reinterpret_cast<volatile LONG*>")
	mustContain(t, text, "g_hold.Drop()", unlocksGlue)
}

func TestTheLinesAreInfoWithTheUserAndTheCount(t *testing.T) {
	t.Parallel()
	text := code(t, unlocksGlue)
	re := regexp.MustCompile(`Log\(EchoVR::LogLevel::(\w+), "\[NEVR\.UNLOCKS\] %s user_id=%llu count=%llu"`)
	var levels []string
	for _, m := range re.FindAllStringSubmatch(text, -1) {
		levels = append(levels, m[1])
	}
	if want := []string{"Info", "Info", "Info", "Info"}; !reflect.DeepEqual(levels, want) { // held, dropped, superseded, re-armed
		t.Errorf("the [NEVR.UNLOCKS] end lines are at %v, want %v", levels, want)
	}
	for _, message := range []string{"kMsgHeld", "kMsgDropped", "kMsgSuperseded", "kMsgRearmed"} {
		mustContain(t, text, message, unlocksGlue)
	}
	if strings.Contains(text, "DIAG") {
		t.Errorf("%s still carries a DIAG line", unlocksGlue)
	}
}

func TestInstalledOnceAtBoot(t *testing.T) {
	t.Parallel()
	if n := strings.Count(code(t, "src/runtime/lifecycle/initialize.cpp"), "nevr_new_unlocks_clear::Install("); n != 1 {
		t.Errorf("initialize.cpp calls nevr_new_unlocks_clear::Install %d times, want once", n)
	}
}

func TestPolledOncePerFrameOnTheGameLoopThread(t *testing.T) {
	t.Parallel()
	text := code(t, "src/runtime/frame/tick.cpp")
	mustContain(t, text, "loop == GetCurrentThreadId()) nevr_new_unlocks_clear::OnFrame();", "tick.cpp")
	if n := strings.Count(text, "nevr_new_unlocks_clear::OnFrame()"); n != 1 {
		t.Errorf("tick.cpp calls nevr_new_unlocks_clear::OnFrame %d times, want once", n)
	}
}

func TestRegisteredInTheBuildAndTheInventory(t *testing.T) {
	t.Parallel()
	cmake := read(t, "src/runtime/CMakeLists.txt")
	mustContain(t, cmake, `"patch/new_unlocks_clear.cpp"`, "src/runtime/CMakeLists.txt")
	mustContain(t, cmake, "add_executable(test_new_unlocks_clear tests/new_unlocks_clear_rules_test.cpp)", "src/runtime/CMakeLists.txt")
	mustContain(t, read(t, "tools/verify_patch_source_inventory.py"), `"patch/new_unlocks_clear.cpp"`, "tools/verify_patch_source_inventory.py")
	mustContain(t, read(t, "justfile"), "test_new_unlocks_clear.exe", "justfile")
}

func TestTheScenarioControlCallsTheClearThroughTheDetour(t *testing.T) {
	t.Parallel()
	mustContain(t, code(t, scenarioCtl), "IsOurDetour(clearTarget)", scenarioCtl)
}

func TestTheTagIsInTheLoggingStandard(t *testing.T) {
	t.Parallel()
	mustContain(t, read(t, "docs/standards/logging.md"), "`[NEVR.UNLOCKS]`", "docs/standards/logging.md")
}

// prologueBytes returns the lower-cased 0x.. bytes of the brace initializer that follows `name =`.
func prologueBytes(t *testing.T, text, name string) []string {
	t.Helper()
	m := regexp.MustCompile(regexp.QuoteMeta(name) + `\s*=\s*\{([^}]*)\}`).FindStringSubmatch(text)
	if m == nil {
		t.Fatalf("no %s = {...}", name)
	}
	var out []string
	for _, b := range regexp.MustCompile(`0x[0-9A-Fa-f]{2}`).FindAllString(m[1], -1) {
		out = append(out, strings.ToLower(b))
	}
	return out
}

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

// code reads a C++ file with its comments stripped.
func code(t *testing.T, path string) string {
	t.Helper()
	return stripComments(read(t, path))
}

// stripComments removes // and /* */ comments from C++ text and keeps string and character literals whole, so a literal
// holding "//" is not cut. A closed block comment leaves one space, as the compiler treats it as white space; newlines are
// kept.
func stripComments(s string) string {
	var out strings.Builder
	const (
		inCode = iota
		line
		block
		str
		chr
	)
	state := inCode
	for i := 0; i < len(s); i++ {
		c := s[i]
		two := ""
		if i+1 < len(s) {
			two = s[i : i+2]
		}
		switch state {
		case line:
			if c == '\n' {
				state = inCode
				out.WriteByte(c)
			}
		case block:
			if two == "*/" {
				state = inCode
				i++
				out.WriteByte(' ')
			} else if c == '\n' {
				out.WriteByte(c)
			}
		case str, chr:
			out.WriteByte(c)
			if c == '\\' && i+1 < len(s) {
				out.WriteByte(s[i+1])
				i++
			} else if (state == str && c == '"') || (state == chr && c == '\'') {
				state = inCode
			}
		default:
			switch {
			case two == "//":
				state = line
				i++
			case two == "/*":
				state = block
				i++
			case c == '"':
				state = str
				out.WriteByte(c)
			case c == '\'':
				state = chr
				out.WriteByte(c)
			default:
				out.WriteByte(c)
			}
		}
	}
	return out.String()
}

func mustContain(t *testing.T, s, want, where string) {
	t.Helper()
	if !strings.Contains(s, want) {
		t.Errorf("%s does not hold %q", where, want)
	}
}

func mustMatch(t *testing.T, re, s, where string) {
	t.Helper()
	if !regexp.MustCompile(re).MatchString(s) {
		t.Errorf("%s does not match %s", where, re)
	}
}

func before(t *testing.T, s, a, b string) {
	t.Helper()
	ia, ib := strings.Index(s, a), strings.Index(s, b)
	if ia < 0 || ib < 0 || ia >= ib {
		t.Errorf("want %q before %q (at %d and %d)", a, b, ia, ib)
	}
}

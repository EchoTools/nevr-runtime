package questwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2410: the new-unlocks clear on Quest is a module of src/quest/combat over the shared rule header
// (src/runtime/patch/new_unlocks_clear_rules.h), built into the sentinel. new_unlocks_clear_test.cpp drives the real wrappers
// through the two installed words, and got_pinned_test.cpp checks the two relocations and the two code pins in the real
// libr15. These pin what neither can see: that the decisions are the shared header's and are not copied into the Quest file,
// that the game's SaveClientProfileChanges and the acknowledgement are not hooked, the combat target, the sentinel's install
// and budget row, the host test's registration, the table the two builds share, and that the module changes no code page.

const nucQuest = "src/quest/combat/new_unlocks_clear.cpp"

func nucHas(t *testing.T, s, want, where string) {
	t.Helper()
	if !strings.Contains(s, want) {
		t.Errorf("%s does not hold %q", where, want)
	}
}

func nucMatch(t *testing.T, re, s, where string) {
	t.Helper()
	if !regexp.MustCompile(re).MatchString(s) {
		t.Errorf("%s does not match %s", where, re)
	}
}

func TestNewUnlocksClearIncludesTheSharedRuleHeader(t *testing.T) {
	t.Parallel()
	nucHas(t, read(t, nucQuest), `#include "runtime/patch/new_unlocks_clear_rules.h"`, nucQuest)
}

func TestNewUnlocksClearHandsTheDecisionToTheRule(t *testing.T) {
	t.Parallel()
	code := stripComments(read(t, nucQuest))
	nucHas(t, body(t, code, "void ClearHandler("), "g_hold.OnClear(", "ClearHandler")
	nucHas(t, body(t, code, "void PollNow() noexcept {"), "g_hold.OnPoll(", "PollNow")
}

func TestNewUnlocksClearCopiesNoDecision(t *testing.T) {
	t.Parallel()
	code := stripComments(read(t, nucQuest))
	for _, pattern := range []string{`\bclass\s+Hold\b`, `countBefore\s*==\s*0`, `\bkStillInFlight\s*=`, `countNow\s*!=\s*0\s*\)\s*\{\s*Drop`} {
		if regexp.MustCompile(pattern).MatchString(code) {
			t.Errorf("%s matches %s: the decision belongs to new_unlocks_clear_rules.h", nucQuest, pattern)
		}
	}
}

func TestNewUnlocksClearRunsTheOriginalBeforeTheRuleIsAsked(t *testing.T) {
	t.Parallel()
	handler := body(t, stripComments(read(t, nucQuest)), "void ClearHandler(")
	if n := strings.Count(handler, "original(netGame);"); n != 1 {
		t.Errorf("ClearHandler calls original(netGame) %d times, want once", n)
	}
	before(t, handler, "original(netGame);", "g_hold.OnClear(")
}

func TestNewUnlocksClearHooksNeitherTheSaveNorTheAcknowledgement(t *testing.T) {
	t.Parallel()
	// libr15.so, libpnsovr.so and libpnsrad.so each define their own copies and call them through their own PLT slots; the poll
	// sets the dirty bit instead, a field every copy reads.
	code := stripComments(read(t, nucQuest))
	for _, name := range []string{"SaveClientProfileChanges", "ProfileUpdateSuccessCB"} {
		if strings.Contains(code, name) {
			t.Errorf("%s names %s", nucQuest, name)
		}
	}
	if n := len(regexp.MustCompile(`InstallThunk<`).FindAllString(code, -1)); n != 1 { // one call site, run once per word
		t.Errorf("%s has %d InstallThunk< call sites, want 1", nucQuest, n)
	}
}

func TestNewUnlocksClearSetsTheDirtyBitAtomicallyAfterTheUserIsRechecked(t *testing.T) {
	t.Parallel()
	poll := body(t, stripComments(read(t, nucQuest)), "void PollNow() noexcept {")
	nucHas(t, poll, "__atomic_fetch_or(", "PollNow")
	before(t, poll, "current != held", "__atomic_fetch_or(")
}

func TestNewUnlocksClearAddressesOfTheSharedTableAreThePinnedOnes(t *testing.T) {
	t.Parallel()
	rules := stripComments(read(t, "src/runtime/patch/new_unlocks_clear_rules.h"))
	pinned := read(t, "src/quest/sentinel/pinned_targets.h")
	nucMatch(t, `kQuestAddresses\s*=\s*\{0x1297114ULL,\s*0xFA82F8ULL\}`, rules, "new_unlocks_clear_rules.h")
	nucMatch(t, `kNewUnlocksClear\s*=\s*\{"_ZN10NRadEngine8NRadGame11CR15NetGame27RemoveAllNewlyUnlockedItemsEv",\s*0x1297114ULL,`, pinned, "pinned_targets.h")
	nucHas(t, pinned, "kJsonEmptyVaddr = 0xfa82f8ULL;", "pinned_targets.h")
	nucHas(t, pinned, "0x36ccac0ULL, 0x3709e08ULL", "pinned_targets.h")
}

func TestNewUnlocksClearLinesAreInfoAndCarryTheUserAndTheCount(t *testing.T) {
	t.Parallel()
	code := stripComments(read(t, nucQuest))
	logEnd := body(t, code, "void LogEnd(")
	nucHas(t, logEnd, "sentinel::LogLevel::kInfo", "LogEnd")
	for _, field := range []string{`"msg"`, `"user_id"`, `"count"`} {
		nucHas(t, logEnd, field, "LogEnd")
	}
	for _, event := range []string{"held", "rearmed", "superseded", "dropped"} {
		nucHas(t, code, `"combat_new_unlocks_clear_`+event+`"`, nucQuest)
	}
}

func TestNewUnlocksClearIsBuiltByTheCombatTarget(t *testing.T) {
	t.Parallel()
	m := regexp.MustCompile(`add_library\(nevr_quest_combat STATIC([^)]*)\)`).FindStringSubmatch(read(t, "src/quest/combat/CMakeLists.txt"))
	if m == nil {
		t.Fatal("src/quest/combat/CMakeLists.txt has no add_library(nevr_quest_combat STATIC ...)")
	}
	found := false
	for _, f := range strings.Fields(m[1]) {
		found = found || f == "new_unlocks_clear.cpp"
	}
	if !found {
		t.Error("nevr_quest_combat does not list new_unlocks_clear.cpp")
	}
}

func TestNewUnlocksClearBudgetHasItsTwoHandles(t *testing.T) {
	t.Parallel()
	nucHas(t, read(t, "src/quest/sentinel/hook_budget.h"), `{"combat/new_unlocks_clear.cpp", 2, 0}`, "hook_budget.h")
	nucMatch(t, `constexpr std::size_t kHookCount = 2;\s*sentinel::GotHook g_hooks\[kHookCount\];`, stripComments(read(t, nucQuest)), nucQuest)
}

// The clear repairs a profile call arena makes too (docs/adr/0046 rule 5), so it installs on every launch from the permission-wait
// step, after the hand pose log and before the permission wait itself, and not from a step the sequence runs only where combat
// runs (docs/adr/0047 decision 3).
func TestNewUnlocksClearInstallsOnEveryLaunch(t *testing.T) {
	t.Parallel()
	entry := stripComments(read(t, "src/quest/sentinel/entry.cpp"))
	const call = "nevr_quest::combat::new_unlocks_clear::Install();"
	wait := body(t, entry, "bool InstallPermissionWaitHook() noexcept {")
	if n := strings.Count(wait, call); n != 1 {
		t.Errorf("InstallPermissionWaitHook calls new_unlocks_clear::Install %d times, want once", n)
	}
	before(t, wait, "nevr_quest::hand_pose_log::Install();", call)
	before(t, wait, call, "return nevr_quest::permission_wait::Install();")
	if n := strings.Count(entry, "new_unlocks_clear::Install("); n != 1 {
		t.Errorf("entry.cpp calls new_unlocks_clear::Install %d times, want once (only from InstallPermissionWaitHook)", n)
	}
}

func TestNewUnlocksClearHostTestIsATargetOfTheCombatGroup(t *testing.T) {
	t.Parallel()
	nucMatch(t, `nevr_ht_test\(new_unlocks_clear_test GROUP combat[^)]*src/quest/tests/new_unlocks_clear_test\.cpp`+
		`\s+src/quest/combat/new_unlocks_clear\.cpp\s+src/quest/invite/frame_dispatch\.cpp\)`,
		read(t, "src/quest/host_tests/groups/combat.cmake"), "combat.cmake")
}

func TestNewUnlocksClearPinnedTestResolvesTheWordsAndTheCode(t *testing.T) {
	t.Parallel()
	text := read(t, "src/quest/tests/got_pinned_test.cpp")
	nucHas(t, text, "pinned::LibR15NewUnlocksClear(kind)", "got_pinned_test.cpp")
	nucHas(t, text, "pinned::kNewUnlocksClearCode[i]", "got_pinned_test.cpp")
}

func TestNewUnlocksClearMutantListExistsForTheRunner(t *testing.T) {
	t.Parallel()
	nucHas(t, read(t, "src/quest/combat/mutants.d/new_unlocks_clear.sh"), "runm nuc_original_skipped", "mutants.d/new_unlocks_clear.sh")
}

func TestNewUnlocksClearChangesNoCodePage(t *testing.T) {
	t.Parallel()
	// N151: the sentinel only stores data into libr15 (the GOT words through the hook manager).
	code := stripComments(read(t, nucQuest))
	for _, call := range []string{"mprotect", "mmap", "mremap", "PROT_EXEC", "PROT_WRITE", "WriteWord", "InstallInlineThunk"} {
		if strings.Contains(code, call) {
			t.Errorf("%s names %s", nucQuest, call)
		}
	}
}

func TestNewUnlocksClearHeaderNamesTheRulesItDoesNotFollow(t *testing.T) {
	t.Parallel()
	text := read(t, "src/quest/combat/new_unlocks_clear.h")
	nucHas(t, text, "ships to every APK", "new_unlocks_clear.h")
	nucHas(t, text, "not gated by combat::Active()", "new_unlocks_clear.h")
}

func TestNewUnlocksClearLoggingStandardListsTheEvents(t *testing.T) {
	t.Parallel()
	var rows []string
	for _, line := range strings.Split(read(t, "docs/standards/logging.md"), "\n") {
		if strings.HasPrefix(line, "| `[NEVR.UNLOCKS]`") {
			rows = append(rows, line)
		}
	}
	if len(rows) != 1 {
		t.Fatalf("docs/standards/logging.md has %d [NEVR.UNLOCKS] rows, want 1", len(rows))
	}
	for _, event := range []string{"combat_new_unlocks_clear_install", "combat_new_unlocks_clear_held", "combat_new_unlocks_clear_rearmed"} {
		nucHas(t, rows[0], event, "the [NEVR.UNLOCKS] row")
	}
}

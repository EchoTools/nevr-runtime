package combatwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #2349: NEUTRON's effects drain (the queue's shape and kinds, Sounds::Again, Drain::Plan and the exact-entry check as an Env input) is
// one definition, src/runtime/patch/ecfx_rules.h, which test_ecfx_rules.cpp drives on PC and ecfx_test.cpp on Quest. dronefix.cpp hooks
// the game and links no test binary, so these pin its wiring: WallFxDue drains through the shared rule with the entry check off and runs
// each entry the rule hands it, the drain is still reached only from the follow update with DRONE on and returns on a server, the Quest's
// ecfx.cpp drains through the same rule with its entries filled in, and neither port keeps a copy of a rule.

const (
	questEcfx     = "src/quest/combat/ecfx.cpp"
	questEcfxCore = "src/quest/combat/ecfx_core.h"
	ecfxRules     = `#include "runtime/patch/ecfx_rules.h"`
)

func TestBothPortsIncludeTheSharedEcfxRules(t *testing.T) {
	t.Parallel()
	for _, path := range []string{dronefix, questEcfxCore} {
		if !strings.Contains(code(t, path), ecfxRules) {
			t.Errorf("%s no longer includes the shared NEUTRON effects rules", path)
		}
	}
	if !strings.Contains(code(t, questEcfx), `#include "quest/combat/ecfx_core.h"`) {
		t.Errorf("%s no longer includes ecfx_core.h, which names the shared rules", questEcfx)
	}
}

func TestWallFxDueDrainsThroughTheSharedRuleWithTheEntryCheckOff(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, dronefix), "void WallFxDue()")
	ordered(t, "WallFxDue", fn,
		"if (g_is_server) return;",
		"FX_QUEUE_RVA",
		"fx::Env env;",
		"env.wallOf = &WallOf;",
		"const ULONGLONG now = GetTickCount64();",
		"g_wall_drain.Plan(q, env, now, [](const fx::Run &r) {",
		"RunWallFx(reinterpret_cast<void *>(r.ctx), static_cast<uintptr_t>(r.fn));",
		"}, &c);",
		"const bool bad = c.refused || c.nowall;",
		"if (!bad && !g_wall_lines_pending.load(std::memory_order_relaxed)) return;",
		"AcquireSRWLockExclusive(&g_wall_lines_lock);",
		"g_wall_refused_total += static_cast<uint64_t>(c.refused);",
		"g_wall_nowall_total += static_cast<uint64_t>(c.nowall);",
		"const nevr::BurstThenCount::Line l = bad ? g_wall_lines.Event(now) : g_wall_lines.Flush(now);",
		"g_wall_lines_pending.store(g_wall_lines.Pending(), std::memory_order_relaxed);",
		"ReleaseSRWLockExclusive(&g_wall_lines_lock);",
		"ReportWallRefusals(l, c, refused, nowall);")
	if strings.Contains(fn, "env.entries") {
		t.Errorf("WallFxDue sets an entry table: PC resolves no entry address, so its exact-entry check is off (#2349 D1):\n%s", fn)
	}
	if strings.Count(fn, "RunWallFx(") != 1 {
		t.Errorf("WallFxDue runs an entry other than the ones the shared drain hands it:\n%s", fn)
	}
}

// The refusal Warnings are never silenced (#2349, logging.md Rule 5): the first 16 drains each, the cap line, then the count with the
// running totals; the budget is 16 lines and 10 s.
func TestTheRefusalWarningsSayWhenTheyStopAndCount(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	if !strings.Contains(src, "nevr::BurstThenCount g_wall_lines{16, 10000};") {
		t.Error("the NEUTRON refusal Warnings no longer have their 16-line, 10 s budget")
	}
	ordered(t, "ReportWallRefusals", body(t, src, "void ReportWallRefusals("),
		"if (l.verdict == V::kWrite)",
		"LogWarning(\"NEUTRON effects freed and not run in one drain: refused=%d",
		"c.refused, c.nowall, static_cast<unsigned long long>(l.total));",
		"else if (l.verdict == V::kCapped)",
		"later drains are counted, not logged, and the count follows at most every 10 s",
		"static_cast<unsigned long long>(l.total), static_cast<unsigned long long>(refused), static_cast<unsigned long long>(nowall));",
		"else if (l.verdict == V::kCount)",
		"since the last line, counted, not logged) refused=%llu nowall=%llu in all",
		"static_cast<unsigned long long>(l.total), static_cast<unsigned long long>(l.sinceLine), static_cast<unsigned long long>(refused),")
}

// The follow update's fault Warnings, a sibling of the shape: the first 5, the cap line, then the count; flushed on every follow update,
// the early return included, and one atomic load when there is nothing to say.
func TestTheFollowFaultWarningsSayWhenTheyStopAndCount(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	if !strings.Contains(src, "nevr::BurstThenCount g_fault_lines{5, 10000};") {
		t.Error("the follow update's fault Warnings no longer have their 5-line, 10 s budget")
	}
	if strings.Contains(src, "InterlockedIncrement(&g_faults) <= 5") {
		t.Error("the follow update's fault Warnings stop at 5 again, with no count")
	}
	ordered(t, "FaultLines", body(t, src, "void FaultLines("),
		"if (!faulted && !g_fault_lines_pending.load(std::memory_order_relaxed)) return;",
		"AcquireSRWLockExclusive(&g_fault_lines_lock);",
		"const nevr::BurstThenCount::Line l = faulted ? g_fault_lines.Event(now) : g_fault_lines.Flush(now);",
		"g_fault_lines_pending.store(g_fault_lines.Pending(), std::memory_order_relaxed);",
		"ReleaseSRWLockExclusive(&g_fault_lines_lock);",
		"if (l.verdict == V::kWrite)",
		"LogWarning(\"follow update faulted and was contained",
		"else if (l.verdict == V::kCapped)",
		"later ones are counted, not logged",
		"else if (l.verdict == V::kCount)",
		"since the last line, counted, not logged")
	update := body(t, src, "void __fastcall Hooked_FollowUpdate(")
	ordered(t, "Hooked_FollowUpdate", update,
		"Log(\"follow update skipped: no orbit target set yet (cs %p)\", (void *)cs);",
		"FaultLines(false, cs);",
		"return;",
		"const bool faulted = !CallGuarded(cs);",
		"if (faulted) InterlockedIncrement(&g_faults);",
		"FaultLines(faulted, cs);")
}

// The Quest's ran and drain records, the other sibling: the first 96 in all, the cap record, then the count; no record path writes
// past the budget.
func TestTheQuestDrainRecordsSayWhenTheyStopAndCount(t *testing.T) {
	t.Parallel()
	src := code(t, questEcfx)
	for _, s := range []string{"constexpr unsigned kLineCap = 96;", "nevr::BurstThenCount g_lineBudget{kLineCap, kLineEveryMs};", "constexpr std::uint64_t kLineEveryMs = 10000;"} {
		if !strings.Contains(src, s) {
			t.Errorf("%s no longer has %q", questEcfx, s)
		}
	}
	if strings.Contains(src, "g_lines") {
		t.Errorf("%s has a line counter outside the budget", questEcfx)
	}
	tick := body(t, src, "void TickOnce(")
	if got := strings.Count(tick, "if (LineDue(now))"); got != 3 {
		t.Errorf("TickOnce gates %d records on the budget, want 3 (ran, not run, drain):\n%s", got, tick)
	}
	if got := strings.Count(tick, "sentinel::LogFields("); got != 4 {
		t.Errorf("TickOnce writes %d records, want 4 (the 3 the budget gates and the gate's own, once):\n%s", got, tick)
	}
	ordered(t, "TickOnce", tick, "}, &c);", "BudgetLine(g_lineBudget.Flush(now));")
	ordered(t, "BudgetLine", body(t, src, "void BudgetLine("),
		"if (l.verdict == nevr::BurstThenCount::Verdict::kCapped) {",
		"\"combat_ecfx_lines\"",
		"} else if (l.verdict == nevr::BurstThenCount::Verdict::kCount) {",
		"\"combat_ecfx_sum\"")
}

// Free stores the word, then the node: the writer claims only a slot whose node is 0, so a node freed first could be claimed while
// the old word is still there. A single-threaded test cannot see the order; this pins it.
func TestFreeStoresTheWordBeforeTheNode(t *testing.T) {
	t.Parallel()
	ordered(t, "Free", body(t, code(t, "src/runtime/patch/ecfx_rules.h"), "inline void Free("),
		"__atomic_store_n(q + 2 * i + 1, uint64_t{0}, __ATOMIC_RELEASE);",
		"__atomic_store_n(q + 2 * i, uint64_t{0}, __ATOMIC_RELEASE);")
}

// The Quest's drain has one caller, RunTick, the single runner the net tick calls (the call site the host mutant ecfx_tick_slot holds).
func TestTheQuestDrainHasOneCaller(t *testing.T) {
	t.Parallel()
	src := code(t, questEcfx)
	if got := len(regexp.MustCompile(`\bTickOnce\(`).FindAllStringIndex(src, -1)); got != 2 {
		t.Errorf("%s names TickOnce %d times, want 2 (its definition and RunTick's call)", questEcfx, got)
	}
	ordered(t, "RunTick", body(t, src, "void RunTick("),
		"if (g_running.test_and_set(std::memory_order_acquire)) return;",
		"TickOnce(now);",
		"g_running.clear(std::memory_order_release);")
}

// WallOf reads [[ctx]+0x68] and returns 0, the rule's read-failure value, when the read faults (#2349 D3).
func TestWallOfReadsTheWallAndReturnsZeroOnAFault(t *testing.T) {
	t.Parallel()
	ordered(t, "WallOf", body(t, code(t, dronefix), "uint64_t WallOf(uint64_t ctx)"),
		"EC_TRY {",
		"return *reinterpret_cast<const uint64_t *>(*reinterpret_cast<uint8_t *const *>(ctx) + 0x68);",
		"} EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { return 0; }")
}

// Arena: the drain is reached only from the follow update's DRONE-on block, as before the move (#2349 step 3).
func TestTheDrainIsReachedOnlyFromTheFollowUpdateWithDroneOn(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	if got := strings.Count(src, "WallFxDue();"); got != 1 {
		t.Errorf("dronefix.cpp calls WallFxDue %d times, want 1 (the follow update, with DRONE on)", got)
	}
	update := body(t, src, "void __fastcall Hooked_FollowUpdate(")
	droneOn := body(t, update, "if (g_drone_on) {")
	if !strings.Contains(droneOn, "WallFxDue();") {
		t.Errorf("WallFxDue is no longer called inside Hooked_FollowUpdate's DRONE-on block:\n%s", update)
	}
	if got := strings.Count(src, "g_wall_drain.Plan("); got != 1 {
		t.Errorf("dronefix.cpp drains the queue %d times, want 1 (WallFxDue)", got)
	}
}

func TestTheQuestDrainsThroughTheSharedRuleWithItsEntriesFilled(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, questEcfx), "void TickOnce(")
	ordered(t, "TickOnce", fn,
		"if (g_lastMs && now - g_lastMs < kEveryMs) return;",
		"Env env;",
		"env.entries = &g_entries;",
		"env.wallOf = &WallOf;",
		"g_drain.Plan(Queue(), env, now, [now](const Run& r) {",
		"if (Named(r.fn, r.kind)) {",
		"reinterpret_cast<EntryFn>(r.fn)(reinterpret_cast<void*>(r.ctx));",
		"}, &c);")
	if strings.Contains(fn, "Free(") {
		t.Errorf("TickOnce frees a slot itself: the shared drain frees each slot after its entry returned (#2349 D5):\n%s", fn)
	}
}

func TestNeitherPortKeepsACopyOfTheDrainRules(t *testing.T) {
	t.Parallel()
	pc := code(t, dronefix)
	for _, literal := range []string{"SoundAgain", "g_wall_sounds", "WallSound", "FX_QUEUE_LEN", "FX_BOOM", "FX_SOUND =", "FX_SOUND)", "now - last > 500", "< 3000) return true"} {
		if strings.Contains(pc, literal) {
			t.Errorf("%s holds %q, which belongs to src/runtime/patch/ecfx_rules.h", dronefix, literal)
		}
	}
	quest := code(t, questEcfx) + code(t, questEcfxCore)
	for _, re := range []string{`\bstruct\s+(Sounds|Drain|Entries|Counts|Run|Env)\b`, `\bkStaleMs\s*=`, `\bkSoundMs\s*=`, `\bkSlots\s*=`, `\bbool\s+Again\(`, `\bint\s+Plan\(`} {
		if regexp.MustCompile(re).MatchString(quest) {
			t.Errorf("the Quest's ecfx keeps a rule of its own (%s), which belongs to src/runtime/patch/ecfx_rules.h", re)
		}
	}
	if _, err := os.Stat(filepath.Join(root(t), "src/quest/combat/ecfx_rules.h")); err == nil {
		t.Error("a Quest copy of ecfx_rules.h exists: the rule is the shared header under src/runtime/patch")
	}
}

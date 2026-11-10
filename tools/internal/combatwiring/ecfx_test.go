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
		"g_wall_drain.Plan(q, env, GetTickCount64(), [](const fx::Run &r) {",
		"RunWallFx(reinterpret_cast<void *>(r.ctx), static_cast<uintptr_t>(r.fn));",
		"}, &c);",
		"if (c.refused || c.nowall) {",
		"const unsigned line = g_wall_refusals.fetch_add(1, std::memory_order_relaxed) + 1;",
		"if (line <= 16)",
		"LogWarning(\"NEUTRON effects freed and not run in one drain: refused=%d",
		"c.refused, c.nowall, line);")
	if strings.Contains(fn, "env.entries") {
		t.Errorf("WallFxDue sets an entry table: PC resolves no entry address, so its exact-entry check is off (#2349 D1):\n%s", fn)
	}
	if strings.Count(fn, "RunWallFx(") != 1 {
		t.Errorf("WallFxDue runs an entry other than the ones the shared drain hands it:\n%s", fn)
	}
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
		"g_drain.Plan(Queue(), env, now, [](const Run& r) {",
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

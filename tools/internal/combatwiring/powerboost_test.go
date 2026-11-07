package combatwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2339: POWER BOOST's cone and link rules (the marker decode, Rotate and AngleTo, the angle in the radius and the bands, the cone
// decision and its bubble table, LeftSeconds and the link marker) are one definition, src/runtime/patch/powerboost_rules.h, which
// test_powerboost_rules.cpp drives on PC and powerboost_test.cpp on Quest. PC's powerboost.cpp hooks the game and links no test binary,
// so these pin its wiring: Hooked_Heal decodes through the shared rule and acts on the unlink kind first, LinkState builds its marker
// through it, ConeFilter classifies and decides through it, and neither port keeps a copy of a rule.

const (
	pcPowerboost    = "src/runtime/combat/powerboost.cpp"
	questPowerboost = "src/quest/combat/powerboost.cpp"
	questPbCore     = "src/quest/combat/powerboost_core.h"
	powerboostRules = `#include "runtime/patch/powerboost_rules.h"`
)

// ordered fails unless every part is in fn, in the order given.
func ordered(t *testing.T, name, fn string, parts ...string) {
	t.Helper()
	at := -1
	for _, p := range parts {
		i := strings.Index(fn, p)
		if i < 0 {
			t.Errorf("%s no longer has %q:\n%s", name, p, fn)
			return
		}
		if i < at {
			t.Errorf("%s has %q before the line above it in this list:\n%s", name, p, fn)
			return
		}
		at = i
	}
}

func TestBothPortsIncludeTheSharedPowerBoostRules(t *testing.T) {
	t.Parallel()
	for _, path := range []string{pcPowerboost, questPowerboost} {
		if !strings.Contains(code(t, path), powerboostRules) {
			t.Errorf("%s no longer includes the shared POWER BOOST rules", path)
		}
	}
}

func TestHookedHealDecodesThroughTheSharedRuleAndActsOnUnlinkFirst(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, pcPowerboost), "void __fastcall Hooked_Heal(")
	ordered(t, "Hooked_Heal", fn,
		"const pbr::Marker m = pbr::Decode(amount);",
		"if (m.kind == pbr::kUnlink) {",
		"if (g_server) EndBuff(cs, (uint16_t)index);",
		"return;",
		"if (m.kind == pbr::kUse) {",
		"g_dr_permille = m.drPermille;",
		"g_dr_seconds = m.seconds;",
		"AddBuff(cs, (uint16_t)index, static_cast<float>(g_dr_permille) / 1000.0f, g_dr_seconds, false, true);",
		"1.0f - static_cast<float>(g_dr_permille) / 1000.0f, (unsigned)g_dr_seconds,",
		"if (m.kind == pbr::kLink) {",
		"AddBuff(cs, (uint16_t)index, static_cast<float>(m.drPermille) / 1000.0f, m.seconds, rez, false);",
		"Real_Heal(cs, index, amount, a4, a5);")
}

func TestLinkStateBuildsItsMarkerThroughTheSharedRule(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, pcPowerboost), "void LinkState(")
	ordered(t, "LinkState", fn,
		"const pbr::UseState use{ g_used_at, g_dr_permille, g_dr_seconds, g_rez };",
		"const unsigned left = pbr::LeftSeconds(use, now);",
		"if (g_server) {",
		"if (linked) AddBuff(dcs, (uint16_t)index, static_cast<float>(use.drPermille) / 1000.0f, left, use.rez, false);",
		"const float marker = pbr::LinkMarker(use, linked, now);",
		"if (marker == 0.0f) return;",
		"SendDamageDealt(dcs, index, -marker, 0, 0);")
}

func TestConeFilterClassifiesAndDecidesThroughTheSharedRule(t *testing.T) {
	t.Parallel()
	src := code(t, pcPowerboost)
	fn := body(t, src, "int __fastcall ConeFilter(")
	ordered(t, "ConeFilter", fn,
		"const int angle = pbr::ConeAngle(radius);",
		"const pbr::Bubble kind = pbr::BubbleOf(angle);",
		"bool medigun = kind == pbr::Bubble::kMedigun;",
		"if (kind == pbr::Bubble::kNeither) return 1;",
		"if (dist < pbr::kOwnerMinDist) return 0;",
		"float range = pbr::ConeRange(radius);",
		"if (medigun) return MediCone(",
		"pbr::ConeDecide(g_cone_table, reinterpret_cast<uintptr_t>(bubble), reinterpret_cast<uintptr_t>(dcs),",
		"index, radius, angle, dist, off, now0);",
		"const bool ok = verdict.ok, send = verdict.send;",
		"if (send) LinkState(dcs, index, ok);",
		"return ok;")
	if !strings.Contains(body(t, src, "BubbleLinks &BubbleFor("), "return g_cone_table.For(reinterpret_cast<uintptr_t>(bubble), now);") {
		t.Error("BubbleFor no longer returns the shared table's entry")
	}
	// MEDIGUN (PC only) keeps its one target in the same table, whose dcs is an address
	ordered(t, "MediCone", body(t, src, "int MediCone(uint8_t *bubble, float off, float dist, float range, int angle, void *dcs, uint32_t index) {"),
		"BubbleLinks &bl = BubbleFor(bubble, now);",
		"bool mine = bl.count && bl.linked[0].dcs == reinterpret_cast<uintptr_t>(dcs) && (bl.linked[0].index & 0xffff) == (index & 0xffff);",
		"bl.linked[0] = { reinterpret_cast<uintptr_t>(dcs), index, true, false, 0 };")
}

func TestThePCServerHalfKeepsItsDefaultAndTheHitBoostSkipsMarkers(t *testing.T) {
	t.Parallel()
	src := code(t, pcPowerboost)
	if !strings.Contains(src, "volatile unsigned g_dr_permille = 90;") {
		t.Error("the damage reduction's default is no longer 90 permille (0.09)")
	}
	if !strings.Contains(body(t, src, "void __fastcall Hooked_Send("), "amount > 0 && amount < pbr::kMarkerMin && GetTickCount64() < g_boosted_until") {
		t.Error("Hooked_Send no longer boosts only hits below the least marker")
	}
}

func TestTheQuestConeLeavesEveryBubbleButPowerBoostsToTheGame(t *testing.T) {
	t.Parallel()
	src := code(t, questPowerboost)
	fn := body(t, src, "const std::uint8_t* ConeAt(const PhTarget* target, const std::uint8_t* frame, std::uintptr_t dcs) noexcept {")
	ordered(t, "ConeAt", fn,
		"const int angle = ConeAngle(radius);",
		"if (BubbleOf(angle) != Bubble::kPowerBoost) return body;",
		"if (dist < kOwnerMinDist) return Reject();",
		"const ConeVerdict v = ConeDecide(g_coneTable, record, dcs, index, radius, angle, dist, off, now);")
	if !strings.Contains(body(t, src, "void NetTick() noexcept"), "const float marker = LinkMarker(use, p[i].linked, now);") {
		t.Error("NetTick no longer builds its marker with the shared LinkMarker")
	}
}

func TestNeitherPowerBoostPortKeepsACopyOfARule(t *testing.T) {
	t.Parallel()
	copies := []*regexp.Regexp{
		regexp.MustCompile(`\bMarker\s+Decode\s*\(\s*float`),
		regexp.MustCompile(`\bint\s+Marker\s*\(`),
		regexp.MustCompile(`\bvoid\s+Rotate\s*\(`),
		regexp.MustCompile(`\bfloat\s+AngleTo\s*\(`),
		regexp.MustCompile(`\bint\s+ConeAngle\s*\(`),
		regexp.MustCompile(`\(\s*radius\s*-\s*floorf\(\s*radius\s*\)\s*\)`),
		regexp.MustCompile(`\bConeVerdict\s+ConeDecide\s*\(`),
		regexp.MustCompile(`\bstruct\s+(BubbleLinks|ConeTable|ConeLink|UseState)\b`),
		regexp.MustCompile(`\b(uint32_t|unsigned)\s+LeftSeconds\s*\(`),
		regexp.MustCompile(`\bfloat\s+LinkMarker\s*\(`),
		regexp.MustCompile(`\b(MARKER_BASE|MARKER_END|LINK_BASE|LINK_END|REZ_FLAG|UNLINK_BASE|UNLINK_END|CONE_SLACK_DEG|LINK_MIN_MS)\b`),
		regexp.MustCompile(`\bk(MarkerMin|MarkerEnd|UseBase|LinkBase|UnlinkBase|UnlinkEnd|RezFlag|ConeSlackDeg|LinkMinMs|BubbleReuseMs|OwnerMinDist|UseWindowMs)\s*=`),
		regexp.MustCompile(`57\.29578f`),
		regexp.MustCompile(`\+\s*999\)\s*/\s*1000`),
		regexp.MustCompile(`>\s*30000\b`),
		regexp.MustCompile(`dist\s*<\s*0\.3f`),
	}
	for _, path := range []string{pcPowerboost, questPowerboost, questPbCore} {
		src := code(t, path)
		for _, re := range copies {
			if loc := re.FindString(src); loc != "" {
				t.Errorf("%s holds %q, which belongs to src/runtime/patch/powerboost_rules.h", path, loc)
			}
		}
	}
}

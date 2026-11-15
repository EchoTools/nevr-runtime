package combatwiring

import (
	"strings"
	"testing"
)

// #2242 (EchoCombat 2.0.3): the RULES and PAUSE asks (POINT RATE, PAYLOAD SPEED, PAUSE / UNPAUSE, SERIES-10 ONLY, LOADOUTS)
// count in a private match only, from the session type the runtime stores; the MAP SWITCH ask is not one of them (it checks
// its own privacy). This pins the server's Handle: each ask branch is under rule_ask, and a refused ask is logged.
func TestVeilnetRuleAsksCountInAPrivateMatchOnly(t *testing.T) {
	src := code(t, veilnet)
	handle := body(t, src, "void Handle(SOCKET s, const Packet &p, const sockaddr_in &from)")
	if !strings.Contains(handle, "const bool rule_ask = privacy == nevr_combat::MatchPrivacy::kPrivate;") {
		t.Error("Handle does not take rule_ask from the match privacy")
	}
	for _, gate := range []string{
		"if (rule_ask && (p.id & (WANT_CHASSIS | WANT_RATE)) == WANT_RATE",
		"if (rule_ask && (p.id & (WANT_CHASSIS | WANT_RATE | WANT_PAUSE)) == WANT_PAUSE)",
		"if (rule_ask && (p.id & WANT_CHASSIS))",
		"if (rule_ask && p.id <= LOADOUT_NEW)",
	} {
		if !strings.Contains(handle, gate) {
			t.Errorf("the RULES / PAUSE ask is not gated by rule_ask: %s", gate)
		}
	}
	if !strings.Contains(handle, "if (p.type == 'C' && !rule_ask && (p.id & WANT_KIND_MASK) != WANT_MAP)") {
		t.Error("a refused RULES ask is not logged (or the MAP SWITCH ask is wrongly refused by it)")
	}
}

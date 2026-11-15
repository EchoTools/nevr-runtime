package combatwiring

import (
	"strings"
	"testing"
)

// #2242: the server's 'G' word, a client's 'G' and 'R' writes into the RULES mailbox, the clear on a server without EchoCombat and
// the 'L' map indices are one definition, src/runtime/combat/match_control_rules.h, which test_veilnet_rules.cpp (PC) and ec_test.cpp
// (Quest) drive. veilnet.cpp hooks the game and links no test binary, so these pin that both clients go through the header, behind
// their check that the datagram is the current server's, and that neither keeps its own decode of the word.

const matchControlRules = "src/runtime/combat/match_control_rules.h"

func TestVeilnetPacksTheRulesWordThroughTheHeader(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, veilnet), "uint32_t RulesWord() {")
	if !strings.Contains(fn, "return nevr_combat::match_control::Pack(r);") || strings.Contains(fn, "<<") {
		t.Errorf("RulesWord packs the word itself:\n%s", fn)
	}
}

func TestVeilnetsClientBranchesWriteTheMailboxThroughTheHeader(t *testing.T) {
	t.Parallel()
	src := code(t, veilnet)
	handle := body(t, src, "void Handle(SOCKET s, const Packet &p, const sockaddr_in &from) {")
	g := handle[strings.Index(handle, "} else if (p.type == 'G') {"):]
	g = g[:strings.Index(g, "} else {")]
	ordered(t, "the 'G' branch", g,
		"if (g_rules && ServerDest(d) && Same(d.addr, from)) {",
		"const mc::Applied a = mc::Apply(g_rules, p.id);",
		"const mc::RulesWord w = mc::Decode(p.id);",
		"PageTexts();")
	if strings.Contains(g, "p.id >>") || strings.Contains(g, "g_rules[") {
		t.Errorf("the 'G' branch decodes or writes the mailbox itself:\n%s", g)
	}
	r := handle[strings.Index(handle, "} else if (p.type == 'R') {"):]
	r = r[:strings.Index(r, "} else if (p.type == 'G') {")]
	ordered(t, "the 'R' branch", r, "ServerDest(d) && Same(d.addr, from)", "nevr_combat::match_control::ApplyRoundTotal(g_rules, p.id)")
	l := handle[strings.Index(handle, "} else if (p.type == 'L') {"):]
	l = l[:strings.Index(l, "} else if (p.type == 'R') {")]
	if !strings.Contains(l, "nevr_combat::match_control::MapIndexValid(p.id)") {
		t.Errorf("the 'L' branch checks the map index itself:\n%s", l)
	}
	if !strings.Contains(body(t, src, "void UpdateGearLists() {"), "nevr_combat::match_control::ClearServerRules(g_rules);") {
		t.Error("UpdateGearLists no longer clears the server's RULES through the header")
	}
}

func TestTheQuestClientWritesTheMailboxThroughTheHeaderFromTheServerOnly(t *testing.T) {
	t.Parallel()
	src := code(t, questEc)
	if !strings.Contains(src, `#include "runtime/combat/match_control_rules.h"`) {
		t.Fatalf("%s does not include the shared match control rules", questEc)
	}
	recv := body(t, src, "__attribute__((noinline)) ssize_t RecvInScope(")
	ordered(t, "RecvInScope", recv,
		"LockG l;",
		"killed = g_st.Handle(sentinel::EcPage(), p, now);",
		"if (IsMatchControl(p.type)) fromServer = FromServerLocked(",
		"if (fromServer) ServerRules(p);")
	ordered(t, "FromServerLocked", body(t, src, "bool FromServerLocked("),
		"g_st.Server(&d) && g_sentTo.SentRecently(d, now100 / kMsIn100ns) && d.addr == from.sin_addr.s_addr && d.port == from.sin_port")
	rules := body(t, src, "void ServerRules(const Packet& p) noexcept {")
	for _, want := range []string{"mc::Apply(Mailbox(), p.id)", "mc::ApplyRoundTotal(Mailbox(), p.id)", "mc::MapIndexValid(p.id)"} {
		if !strings.Contains(rules, want) {
			t.Errorf("ServerRules does not call %q", want)
		}
	}
	if strings.Contains(rules, "p.id >>") || strings.Contains(rules, "p.id &") {
		t.Errorf("ServerRules decodes the word itself:\n%s", rules)
	}
	if !strings.Contains(body(t, src, "void Step(std::uint64_t now100) noexcept {"), "if (!ans && mc::ClearServerRules(page + kRulesMailbox)") {
		t.Error("Step no longer clears the server's RULES through the header on a server that does not state EchoCombat")
	}
	if !strings.Contains(code(t, questEcCore), "kRulesMailbox = static_cast<std::uint32_t>(::nevr_combat::rules::kMailboxRva) - ::nevr_ec::kPageRva;") {
		t.Error("the Quest's mailbox is no longer rules_state.h's RVA on the EC page")
	}
}

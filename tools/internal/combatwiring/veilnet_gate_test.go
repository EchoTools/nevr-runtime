package combatwiring

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// #2242 (EchoCombat 2.0.6): VeilNet picks as the server only a destination the game still sends to (the send time per
// destination), sends only the hello to a server that has not answered, and stops the hellos after kHelloTries unanswered ones.
// The decisions are combat/veilnet_gate.h (test_veilnet_gate.cpp drives them); this pins that veilnet.cpp takes them.
func TestVeilnetSendsOnlyToTheServerTheGameStillTalksTo(t *testing.T) {
	src := code(t, veilnet)
	sawSend := body(t, src, "void SawSend(SOCKET s, const sockaddr *to) {")
	if !strings.Contains(sawSend, "NoteSent(") {
		t.Error("SawSend does not record the game's send time per destination")
	}
	serverDest := body(t, src, "bool ServerDest(Dest &out) {")
	if !strings.Contains(serverDest, "SentRecently(") {
		t.Error("ServerDest gives the picked server without checking the game still sends there")
	}
	sendTo := body(t, src, "void SendToServer(uint8_t type, uint32_t id) {")
	if !strings.Contains(sendTo, "gate::SendAllowed(type, ") || !strings.Contains(sendTo, "nevr_server_features::Client().Current().echoCombat") {
		t.Errorf("SendToServer does not gate on the server's EchoCombat statement (only the hello goes to an unanswered server): %s", sendTo)
	}
	hello := body(t, src, "if (now >= next_hello) {")
	if !strings.Contains(hello, "gate::HelloTries") || !strings.Contains(hello, "kSendAndGiveUp") {
		t.Errorf("the hello tick does not stop after the unanswered tries: %s", hello)
	}
}

// The two constants and the loop bound that the gate's send path depends on, read from the source as written (the declarations,
// not stripped of their values): a change to the mask, to the destination table's size, or to the bound of its loop fails here.
func TestVeilnetWantMaskAndDestinationTableSizeAndBound(t *testing.T) {
	raw := readSource(t, veilnet)
	for _, want := range []string{
		"constexpr uint32_t WANT_KIND_MASK = 0xF0000000;",
		"nevr_combat::gate::SentTo<SOCKET> g_sent_to;",
	} {
		if !strings.Contains(raw, want) {
			t.Errorf("veilnet.cpp no longer declares %q", want)
		}
	}
	hello := body(t, code(t, veilnet), "if (now >= next_hello) {")
	if !strings.Contains(hello, "const uint64_t key = have ? nevr_combat::gate::ServerKey(d.addr.sin_addr.s_addr, d.addr.sin_port) : 0;") {
		t.Error("the hello tick does not key the server by gate::ServerKey (address and port)")
	}
	gateHeader := code(t, "src/runtime/combat/veilnet_gate.h")
	note := body(t, gateHeader, "void Note(Sock sock, std::uint32_t addr, std::uint16_t port, std::uint64_t nowMs) {")
	if !strings.Contains(note, "for (int i = 0; i < kSlots; ++i) {") {
		t.Errorf("the shared sent-to table does not scan the whole destination table: %s", note)
	}
	if !strings.Contains(raw, "g_sent_to.Note(") || !strings.Contains(raw, "g_sent_to.SentRecently(") {
		t.Error("veilnet.cpp does not use the shared sent-to table")
	}
}


// readSource is the file as written, comments included (code() strips them).
func readSource(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(root(t), path))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

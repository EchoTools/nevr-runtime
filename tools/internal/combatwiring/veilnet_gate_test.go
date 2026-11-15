package combatwiring

import (
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

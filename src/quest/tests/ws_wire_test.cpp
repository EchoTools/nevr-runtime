// Host test for the RFC 6455 server-side wire pieces (src/quest/net/ws_wire.{h,cpp}): handshake parsing,
// the accept key, and the incremental frame decoder under partial reads and malformed input.

#include <cstdio>
#include <string>
#include <vector>

#include "quest/net/ws_wire.h"
#include "quest/tests/test_check.h"

using namespace quest_net;

namespace {

const uint8_t kMask[4] = {0x37, 0xfa, 0x21, 0x3d};

std::string Request(const std::string& extra = "", const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==") {
  return "GET /x?y=1 HTTP/1.1\r\nHost: 127.0.0.1:1\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
         "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
}

// The RFC 6455 section 1.3 example.
void TestAcceptKey() {
  QCHECK(ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  QCHECK(BuildUpgradeResponse("dGhlIHNhbXBsZSBub25jZQ==").find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n") !=
         std::string::npos);
  QCHECK(BuildUpgradeResponse("dGhlIHNhbXBsZSBub25jZQ==").rfind("HTTP/1.1 101", 0) == 0);
}

void TestHandshakeParse() {
  UpgradeRequest req;
  const std::string good = Request();
  QCHECK(ParseUpgradeRequest(good, &req) == HandshakeStatus::Ok);
  QCHECK(req.key == "dGhlIHNhbXBsZSBub25jZQ==");
  QCHECK(req.consumed == good.size());

  // Partial reads: every strict prefix needs more bytes, none is accepted early.
  bool allNeedMore = true;
  for (std::size_t n = 0; n < good.size(); ++n) {
    UpgradeRequest tmp;
    if (ParseUpgradeRequest(std::string_view(good).substr(0, n), &tmp) != HandshakeStatus::NeedMore) allNeedMore = false;
  }
  QCHECK(allNeedMore);

  // Frame bytes pipelined behind the request are left alone.
  const std::string withFrame = good + BuildMaskedFrame(Opcode::Binary, "hi", kMask);
  UpgradeRequest piped;
  QCHECK(ParseUpgradeRequest(withFrame, &piped) == HandshakeStatus::Ok);
  QCHECK(piped.consumed == good.size());

  QCHECK(ParseUpgradeRequest("POST / HTTP/1.1\r\n\r\n", &req) == HandshakeStatus::Bad);
  QCHECK(ParseUpgradeRequest(Request("", "short"), &req) == HandshakeStatus::Bad);
  std::string noUpgrade = good;
  noUpgrade.replace(noUpgrade.find("Upgrade: websocket"), 18, "X-Other: websocket");
  QCHECK(ParseUpgradeRequest(noUpgrade, &req) == HandshakeStatus::Bad);
  std::string v8 = good;
  v8.replace(v8.find("Version: 13"), 11, "Version: 8 ");
  QCHECK(ParseUpgradeRequest(v8, &req) == HandshakeStatus::Bad);
  QCHECK(ParseUpgradeRequest("GET / HTTP/1.1\r\nbroken header line\r\n\r\n", &req) == HandshakeStatus::Bad);
  // Header names and tokens are case-insensitive.
  std::string lower = good;
  lower.replace(lower.find("Upgrade: websocket"), 18, "upgrade: WebSocket");
  QCHECK(ParseUpgradeRequest(lower, &req) == HandshakeStatus::Ok);
  // Endless headers are refused, not buffered.
  QCHECK(ParseUpgradeRequest("GET / HTTP/1.1\r\n" + std::string(kMaxHandshakeBytes, 'a'), &req) == HandshakeStatus::TooLarge);
}

std::vector<Message> DecodeAll(FrameDecoder& d, DecodeStatus* last) {
  std::vector<Message> out;
  for (;;) {
    Message m;
    *last = d.Next(&m);
    if (*last != DecodeStatus::Message) return out;
    out.push_back(std::move(m));
  }
}

// Failure caught: a message dropped, duplicated or corrupted when the stream arrives in pieces.
void TestPartialReads() {
  for (const std::size_t size : {std::size_t(0), std::size_t(5), std::size_t(125), std::size_t(126), std::size_t(65535),
                                 std::size_t(65536)}) {
    std::string payload(size, 'a');
    for (std::size_t i = 0; i < size; ++i) payload[i] = static_cast<char>('a' + (i % 26));
    const std::string wire = BuildMaskedFrame(Opcode::Binary, payload, kMask);
    FrameDecoder d(1u << 20);
    int messages = 0;
    bool intact = true;
    for (std::size_t i = 0; i < wire.size(); ++i) {  // one byte at a time
      d.Feed(&wire[i], 1);
      Message m;
      while (d.Next(&m) == DecodeStatus::Message) {
        ++messages;
        if (m.opcode != Opcode::Binary || m.payload != payload) intact = false;
      }
    }
    QCHECK(messages == 1);
    QCHECK(intact);
    QCHECK(d.Buffered() == 0);
  }
}

void TestSeveralFramesInOneFeedAndSplitMidHeader() {
  const std::string a = BuildMaskedFrame(Opcode::Binary, "one", kMask), b = BuildMaskedFrame(Opcode::Text, "two", kMask);
  const std::string wire = a + b + BuildMaskedFrame(Opcode::Binary, std::string(300, 'z'), kMask);
  for (std::size_t split = 0; split <= wire.size(); ++split) {
    FrameDecoder d(1u << 20);
    DecodeStatus last;
    d.Feed(wire.data(), split);
    std::vector<Message> got = DecodeAll(d, &last);
    d.Feed(wire.data() + split, wire.size() - split);
    std::vector<Message> more = DecodeAll(d, &last);
    got.insert(got.end(), more.begin(), more.end());
    if (got.size() != 3 || got[0].payload != "one" || got[1].payload != "two" || got[1].opcode != Opcode::Text ||
        got[2].payload != std::string(300, 'z')) {
      std::fprintf(stderr, "split at %zu produced %zu messages\n", split, got.size());
      QCHECK(false);
      return;
    }
  }
}

void TestFragmentationWithInterleavedPing() {
  FrameDecoder d(1u << 20);
  const std::string wire = BuildMaskedFrame(Opcode::Binary, "AB", kMask, false) +
                           BuildMaskedFrame(Opcode::Ping, "p", kMask) +
                           BuildMaskedFrame(Opcode::Continuation, "CD", kMask, false) +
                           BuildMaskedFrame(Opcode::Continuation, "EF", kMask, true);
  d.Feed(wire.data(), wire.size());
  DecodeStatus last;
  const auto got = DecodeAll(d, &last);
  QCHECK(last == DecodeStatus::NeedMore);
  QCHECK(got.size() == 2);
  if (got.size() == 2) {
    QCHECK(got[0].opcode == Opcode::Ping && got[0].payload == "p");
    QCHECK(got[1].opcode == Opcode::Binary && got[1].payload == "ABCDEF");
  }
}

void TestUnmaskedFrameIsAcceptedAndCounted() {
  FrameDecoder d(1024);
  const std::string wire = BuildFrame(Opcode::Binary, "hello");
  d.Feed(wire.data(), wire.size());
  Message m;
  QCHECK(d.Next(&m) == DecodeStatus::Message);
  QCHECK(m.payload == "hello");
  QCHECK(d.UnmaskedFrames() == 1);
}

void TestCloseFrame() {
  FrameDecoder d(1024);
  std::string payload;
  payload.push_back(static_cast<char>(0x03));
  payload.push_back(static_cast<char>(0xE9));
  payload += "bye";
  const std::string wire = BuildMaskedFrame(Opcode::Close, payload, kMask) + BuildMaskedFrame(Opcode::Close, "", kMask);
  d.Feed(wire.data(), wire.size());
  Message m;
  QCHECK(d.Next(&m) == DecodeStatus::Message);
  QCHECK(m.opcode == Opcode::Close && m.closeCode == 1001 && m.payload.substr(2) == "bye");
  QCHECK(d.Next(&m) == DecodeStatus::Message);
  QCHECK(m.closeCode == 1005);
  FrameDecoder bad(1024);
  const std::string one = BuildMaskedFrame(Opcode::Close, "x", kMask);  // a 1-byte close payload is invalid
  bad.Feed(one.data(), one.size());
  QCHECK(bad.Next(&m) == DecodeStatus::ProtocolError);
}

// Failure caught: a hostile or broken peer making the decoder buffer or accept garbage.
void TestMalformedStreams() {
  Message m;
  {
    FrameDecoder d(1024);  // RSV1 set, no extension negotiated
    const std::string w = std::string("\xC2\x00", 2);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::ProtocolError);
    QCHECK(d.Next(&m) == DecodeStatus::ProtocolError);  // stays failed
  }
  {
    FrameDecoder d(1024);  // opcode 3 is reserved
    const std::string w = std::string("\x83\x00", 2);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::ProtocolError);
  }
  {
    FrameDecoder d(1024);  // a ping longer than 125 bytes
    const std::string w = BuildMaskedFrame(Opcode::Ping, std::string(126, 'x'), kMask);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::ProtocolError);
  }
  {
    FrameDecoder d(1024);  // a fragmented ping
    const std::string w = BuildMaskedFrame(Opcode::Ping, "x", kMask, false);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::ProtocolError);
  }
  {
    FrameDecoder d(1024);  // continuation with nothing to continue
    const std::string w = BuildMaskedFrame(Opcode::Continuation, "x", kMask);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::ProtocolError);
  }
  {
    FrameDecoder d(1024);  // a new data frame while a fragmented message is open
    const std::string w = BuildMaskedFrame(Opcode::Binary, "a", kMask, false) + BuildMaskedFrame(Opcode::Binary, "b", kMask);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::ProtocolError);
  }
  {
    FrameDecoder d(1024);  // 64-bit length with the top bit set
    const std::string w = std::string("\x82\x7f\x80\x00\x00\x00\x00\x00\x00\x00", 10);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::ProtocolError);
  }
}

// Failure caught: buffering a declared-huge payload before checking it, and a limit that only the
// whole message (not each fragment) would trip.
void TestSizeLimits() {
  Message m;
  {
    FrameDecoder d(1000);
    // Header only: declares 2^40 bytes. Refused on the header; nothing of the payload exists to buffer.
    const std::string w = std::string("\x82\x7f\x00\x00\x01\x00\x00\x00\x00\x00", 10);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::TooBig);
    QCHECK(d.Buffered() <= w.size());
  }
  {
    FrameDecoder d(1000);  // exactly at the limit passes, one over does not
    const std::string at = BuildMaskedFrame(Opcode::Binary, std::string(1000, 'a'), kMask);
    d.Feed(at.data(), at.size());
    QCHECK(d.Next(&m) == DecodeStatus::Message);
    const std::string over = BuildMaskedFrame(Opcode::Binary, std::string(1001, 'a'), kMask);
    d.Feed(over.data(), over.size());
    QCHECK(d.Next(&m) == DecodeStatus::TooBig);
  }
  {
    FrameDecoder d(1000);  // fragments that each fit but together do not
    const std::string w = BuildMaskedFrame(Opcode::Binary, std::string(600, 'a'), kMask, false) +
                          BuildMaskedFrame(Opcode::Continuation, std::string(600, 'b'), kMask, true);
    d.Feed(w.data(), w.size());
    QCHECK(d.Next(&m) == DecodeStatus::TooBig);
  }
}

void TestBuildFrameLengths() {
  for (const std::size_t size : {std::size_t(0), std::size_t(125), std::size_t(126), std::size_t(65535), std::size_t(65536)}) {
    const std::string payload(size, 'q');
    const std::string wire = BuildFrame(Opcode::Binary, payload);
    FrameDecoder d(1u << 20);
    d.Feed(wire.data(), wire.size());
    Message m;
    QCHECK(d.Next(&m) == DecodeStatus::Message);
    QCHECK(m.payload == payload);
  }
  const std::string close = BuildCloseFrame(1009, std::string(500, 'r'));
  FrameDecoder d(1024);
  d.Feed(close.data(), close.size());
  Message m;
  QCHECK(d.Next(&m) == DecodeStatus::Message);
  QCHECK(m.opcode == Opcode::Close && m.closeCode == 1009 && m.payload.size() <= 125);
}

}  // namespace

int main() {
  TestAcceptKey();
  TestHandshakeParse();
  TestPartialReads();
  TestSeveralFramesInOneFeedAndSplitMidHeader();
  TestFragmentationWithInterleavedPing();
  TestUnmaskedFrameIsAcceptedAndCounted();
  TestCloseFrame();
  TestMalformedStreams();
  TestSizeLimits();
  TestBuildFrameLengths();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "ws_wire_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("ws_wire_test: all checks passed\n");
  return 0;
}

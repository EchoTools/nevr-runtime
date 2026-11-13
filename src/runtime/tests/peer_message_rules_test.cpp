// Tests for runtime/compat/peer_message_rules.h (#2365): the Envelope codec of nEVR's control messages on the game's
// broadcaster, and each rule that refuses one.
#include "runtime/compat/peer_message_rules.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pm = nevr_peer_message;

namespace {

pm::Decoded DecodeString(const std::string& bytes) { return pm::Decode(bytes.data(), bytes.size()); }

std::string Unknown(std::uint64_t type, const std::string& data) {
  gameservice::v1::Envelope envelope;
  envelope.mutable_unknown_message()->mutable_type()->set_value(type);
  envelope.mutable_unknown_message()->set_data(data);
  return envelope.SerializeAsString();
}

}  // namespace

TEST(PeerMessageCodec, CapabilitiesRoundTrip) {
  const std::string bytes = pm::EncodeCapabilities({"smoke", "buff", "point", "medigun"});
  ASSERT_FALSE(bytes.empty());
  const pm::Decoded d = DecodeString(bytes);
  EXPECT_EQ(d.refusal, pm::Refusal::kNone);
  EXPECT_EQ(d.message.kind, pm::Kind::kCapabilities);
  EXPECT_EQ(d.message.gear, (std::vector<std::string>{"smoke", "buff", "point", "medigun"}));
  EXPECT_EQ(pm::GearText(d.message.gear), "smoke,buff,point,medigun");
}

TEST(PeerMessageCodec, EmptyGearListIsAMessage) {
  const pm::Decoded d = DecodeString(pm::EncodeCapabilities({}));
  EXPECT_EQ(d.refusal, pm::Refusal::kNone);
  EXPECT_EQ(d.message.kind, pm::Kind::kCapabilities);
  EXPECT_TRUE(d.message.gear.empty());
  EXPECT_EQ(pm::GearText(d.message.gear), "none");
}

TEST(PeerMessageCodec, PingAndPongRoundTrip) {
  pm::Decoded d = DecodeString(pm::EncodePing(7));
  EXPECT_EQ(d.refusal, pm::Refusal::kNone);
  EXPECT_EQ(d.message.kind, pm::Kind::kPing);
  EXPECT_EQ(d.message.seq, 7u);
  d = DecodeString(pm::EncodePong(UINT64_MAX));
  EXPECT_EQ(d.refusal, pm::Refusal::kNone);
  EXPECT_EQ(d.message.kind, pm::Kind::kPong);
  EXPECT_EQ(d.message.seq, UINT64_MAX);
}

TEST(PeerMessageCodec, TypesAreTheGameHashesOfTheirNames) {
  EXPECT_EQ(pm::kCapabilitiesType, EchoVR::CSymbol64Hash("nevr_peer_capabilities"));
  EXPECT_NE(pm::kCapabilitiesType, pm::kPingType);
  EXPECT_NE(pm::kPingType, pm::kPongType);
  EXPECT_EQ(pm::kMessageSymbol, 0x9ee5107d9e29fd63ULL);
}

TEST(PeerMessageCodec, EncodeRefusesGearItWouldNotTake) {
  EXPECT_TRUE(pm::EncodeCapabilities({"Smoke"}).empty());
  EXPECT_TRUE(pm::EncodeCapabilities({""}).empty());
  EXPECT_TRUE(pm::EncodeCapabilities({std::string(33, 'a')}).empty());
  EXPECT_TRUE(pm::EncodeCapabilities(std::vector<std::string>(33, "a")).empty());
  EXPECT_FALSE(pm::EncodeCapabilities({std::string(32, 'a')}).empty());
  EXPECT_FALSE(pm::EncodeCapabilities(std::vector<std::string>(32, "a")).empty());
}

TEST(PeerMessageRefusal, EmptyInput) {
  EXPECT_EQ(pm::Decode(nullptr, 5).refusal, pm::Refusal::kEmpty);
  const char b = 0;
  EXPECT_EQ(pm::Decode(&b, 0).refusal, pm::Refusal::kEmpty);
}

TEST(PeerMessageRefusal, Oversize) {
  const std::string big(pm::kMaxEnvelopeBytes + 1, '\0');
  EXPECT_EQ(DecodeString(big).refusal, pm::Refusal::kOversize);
  // A valid Envelope padded past the cap by its body is refused for its size, before it is parsed.
  const std::string padded = Unknown(pm::kPingType, "{\"seq\":1,\"pad\":\"" + std::string(pm::kMaxEnvelopeBytes, 'x') + "\"}");
  EXPECT_EQ(DecodeString(padded).refusal, pm::Refusal::kOversize);
}

TEST(PeerMessageRefusal, AtTheCapIsNotOversize) {
  // The cap itself is taken for size: a message of exactly kMaxEnvelopeBytes reaches the parser.
  std::string bytes;
  for (std::size_t n = 0; n < pm::kMaxEnvelopeBytes; ++n) {
    const std::string next = Unknown(pm::kPingType, "{\"seq\":1,\"pad\":\"" + std::string(n, 'x') + "\"}");
    if (next.size() > pm::kMaxEnvelopeBytes) break;
    bytes = next;
  }
  ASSERT_EQ(bytes.size(), pm::kMaxEnvelopeBytes);
  EXPECT_EQ(DecodeString(bytes).refusal, pm::Refusal::kNone);
  bytes.push_back('\0');
  EXPECT_EQ(DecodeString(bytes).refusal, pm::Refusal::kOversize);
}

TEST(PeerMessageRefusal, Undecodable) {
  const std::string garbage("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 10);
  EXPECT_EQ(DecodeString(garbage).refusal, pm::Refusal::kUndecodable);
}

TEST(PeerMessageRefusal, AnotherEnvelopeArm) {
  gameservice::v1::Envelope envelope;
  envelope.mutable_lobby_session_event()->set_lobby_session_id("x");
  EXPECT_EQ(DecodeString(envelope.SerializeAsString()).refusal, pm::Refusal::kUnexpectedArm);
  // An Envelope with no arm set serializes to no bytes at all.
  EXPECT_TRUE(gameservice::v1::Envelope().SerializeAsString().empty());
}

TEST(PeerMessageRefusal, UnknownType) {
  EXPECT_EQ(DecodeString(Unknown(EchoVR::CSymbol64Hash("nevr_peer_other"), "{}")).refusal, pm::Refusal::kUnknownType);
}

TEST(PeerMessageRefusal, BadBodies) {
  const std::vector<std::pair<std::uint64_t, std::string>> cases = {
      {pm::kPingType, "not json"},
      {pm::kPingType, "[1]"},
      {pm::kPingType, "{}"},
      {pm::kPingType, "{\"seq\":-1}"},
      {pm::kPingType, "{\"seq\":\"1\"}"},
      {pm::kPongType, "{\"seq\":1.5}"},
      {pm::kCapabilitiesType, "{}"},
      {pm::kCapabilitiesType, "{\"gear\":\"smoke\"}"},
      {pm::kCapabilitiesType, "{\"gear\":[1]}"},
      {pm::kCapabilitiesType, "{\"gear\":[\"Smoke\"]}"},
      {pm::kCapabilitiesType, "{\"gear\":[\"smo ke\"]}"},
      {pm::kCapabilitiesType, "{\"gear\":[\"\"]}"},
      {pm::kCapabilitiesType, "{\"gear\":[\"" + std::string(33, 'a') + "\"]}"},
      {pm::kCapabilitiesType, std::string("{\"gear\":[\"\xc3\x28\"]}")},
  };
  for (const auto& [type, body] : cases) {
    const pm::Decoded d = DecodeString(Unknown(type, body));
    EXPECT_EQ(d.refusal, pm::Refusal::kBadBody) << body;
    EXPECT_TRUE(d.message.gear.empty()) << body;
  }
  std::string many = "{\"gear\":[";
  for (int i = 0; i < 33; ++i) many += std::string(i ? "," : "") + "\"a\"";
  many += "]}";
  EXPECT_EQ(DecodeString(Unknown(pm::kCapabilitiesType, many)).refusal, pm::Refusal::kBadBody);
}

TEST(PeerMessageAdmit, ClientTakesOnlyTheAuthorityPeer) {
  EXPECT_EQ(pm::Admit(pm::Role::kClient, pm::Kind::kCapabilities, 5, 5, {}), pm::Refusal::kNone);
  EXPECT_EQ(pm::Admit(pm::Role::kClient, pm::Kind::kCapabilities, 6, 5, {}), pm::Refusal::kWrongSender);
  EXPECT_EQ(pm::Admit(pm::Role::kClient, pm::Kind::kPing, 5, std::nullopt, {}), pm::Refusal::kWrongSender);
  // A match peer list means nothing to a client.
  EXPECT_EQ(pm::Admit(pm::Role::kClient, pm::Kind::kPing, 6, 5, {6}), pm::Refusal::kWrongSender);
}

TEST(PeerMessageAdmit, ServerTakesOnlyPeersInTheMatch) {
  EXPECT_EQ(pm::Admit(pm::Role::kServer, pm::Kind::kPing, 3, std::nullopt, {1, 3}), pm::Refusal::kNone);
  EXPECT_EQ(pm::Admit(pm::Role::kServer, pm::Kind::kPong, 1, std::nullopt, {1, 3}), pm::Refusal::kNone);
  EXPECT_EQ(pm::Admit(pm::Role::kServer, pm::Kind::kPing, 2, std::nullopt, {1, 3}), pm::Refusal::kWrongSender);
  EXPECT_EQ(pm::Admit(pm::Role::kServer, pm::Kind::kPing, 2, std::nullopt, {}), pm::Refusal::kWrongSender);
}

TEST(PeerMessageAdmit, ServerNeverTakesCapabilities) {
  EXPECT_EQ(pm::Admit(pm::Role::kServer, pm::Kind::kCapabilities, 3, std::nullopt, {3}), pm::Refusal::kNotForServer);
}

TEST(PeerMessageGear, FromFeatures) {
  EXPECT_TRUE(pm::GearFromFeatures({}).empty());
  EXPECT_TRUE(pm::GearFromFeatures({"echocombat_medigun"}).empty());
  EXPECT_EQ(pm::GearFromFeatures({"echocombat"}), (std::vector<std::string>{"smoke", "buff", "point"}));
  EXPECT_EQ(pm::GearFromFeatures({"echocombat", "echocombat_medigun"}),
            (std::vector<std::string>{"smoke", "buff", "point", "medigun"}));
}

TEST(PeerMessageLimiter, TenPerWindowThenCounted) {
  pm::RefusalLimiter limiter;
  std::uint64_t suppressed = 99;
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(limiter.Take(1000 + i, &suppressed)) << i;
    EXPECT_EQ(suppressed, 0u);
  }
  EXPECT_FALSE(limiter.Take(1100, &suppressed));
  EXPECT_FALSE(limiter.Take(10999, &suppressed));
  // The next window writes again and carries the two it did not write.
  ASSERT_TRUE(limiter.Take(11000, &suppressed));
  EXPECT_EQ(suppressed, 2u);
  ASSERT_TRUE(limiter.Take(11001, &suppressed));
  EXPECT_EQ(suppressed, 0u);
}

TEST(PeerMessagePingBook, AnswersOnceWithTheRoundTrip) {
  pm::PingBook book;
  const std::uint64_t a = book.Next(100);
  const std::uint64_t b = book.Next(150);
  EXPECT_NE(a, b);
  EXPECT_EQ(book.Answer(b, 190), std::optional<std::uint64_t>(40));
  EXPECT_EQ(book.Answer(b, 200), std::nullopt);  // answered already
  EXPECT_EQ(book.Answer(a, 300), std::optional<std::uint64_t>(200));
  EXPECT_EQ(book.Answer(12345, 300), std::nullopt);  // never sent
}

TEST(PeerMessagePingBook, ForgetsTheOldestPastItsBound) {
  pm::PingBook book;
  const std::uint64_t first = book.Next(0);
  std::uint64_t last = first;
  for (std::size_t i = 0; i < pm::PingBook::kMaxOutstanding; ++i) last = book.Next(10);
  EXPECT_EQ(book.Answer(first, 20), std::nullopt);
  EXPECT_EQ(book.Answer(last, 20), std::optional<std::uint64_t>(10));
}

TEST(PeerMessageAccept, TenPerPeerPerWindowThenCounted) {
  pm::AcceptLimiter limiter;
  std::uint64_t dropped = 99;
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(limiter.Take(7, 1000 + i, &dropped)) << i;
    EXPECT_EQ(dropped, 0u);
  }
  EXPECT_FALSE(limiter.Take(7, 1100, &dropped));
  EXPECT_FALSE(limiter.Take(7, 1200, &dropped));
  EXPECT_FALSE(limiter.Take(7, 1300, &dropped));
  // Another peer has its own window.
  ASSERT_TRUE(limiter.Take(8, 1300, &dropped));
  EXPECT_EQ(dropped, 0u);
  // Peer 7's next window takes again and carries the three it dropped.
  ASSERT_TRUE(limiter.Take(7, 11000, &dropped));
  EXPECT_EQ(dropped, 3u);
  ASSERT_TRUE(limiter.Take(7, 11001, &dropped));
  EXPECT_EQ(dropped, 0u);
}

TEST(PeerMessageAccept, RemembersABoundedNumberOfPeers) {
  pm::AcceptLimiter limiter;
  std::uint64_t dropped = 0;
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(limiter.Take(1, 0, &dropped));
  ASSERT_FALSE(limiter.Take(1, 1, &dropped));  // peer 1 is at its bound
  for (std::uint64_t p = 100; p < 100 + pm::AcceptLimiter::kMaxPeers; ++p) ASSERT_TRUE(limiter.Take(p, 2, &dropped));
  // Peer 1 (seen least recently) was replaced, so it starts a fresh window and its old count is gone.
  ASSERT_TRUE(limiter.Take(1, 3, &dropped));
  EXPECT_EQ(dropped, 0u);
}

TEST(PeerMessageGreet, OncePerPeerPerSession) {
  pm::GreetBook book;
  pm::GreetBook::Step s = book.Update("a", {1, 2});
  EXPECT_FALSE(s.sessionChanged);
  EXPECT_EQ(s.greet, (std::vector<std::uint64_t>{1, 2}));
  s = book.Update("a", {1, 2, 3});
  EXPECT_EQ(s.greet, (std::vector<std::uint64_t>{3}));
  s = book.Update("a", {2, 3});
  EXPECT_TRUE(s.greet.empty());
}

TEST(PeerMessageGreet, ANewSessionGreetsAgain) {
  pm::GreetBook book;
  book.Update("a", {1, 2});
  const pm::GreetBook::Step s = book.Update("b", {1});
  EXPECT_TRUE(s.sessionChanged);
  EXPECT_EQ(s.forgotten, 2u);
  EXPECT_EQ(s.greet, (std::vector<std::uint64_t>{1}));
  EXPECT_EQ(book.session(), "b");
}

TEST(PeerMessageGreet, NobodyBetweenSessions) {
  pm::GreetBook book;
  pm::GreetBook::Step s = book.Update("", {1});
  EXPECT_FALSE(s.sessionChanged);
  EXPECT_TRUE(s.greet.empty());
  book.Update("a", {1});
  s = book.Update("", {1});
  EXPECT_TRUE(s.sessionChanged);
  EXPECT_EQ(s.forgotten, 1u);
  EXPECT_TRUE(s.greet.empty());
  // The same peer in the next session is greeted again.
  s = book.Update("b", {1});
  EXPECT_EQ(s.greet, (std::vector<std::uint64_t>{1}));
}

TEST(PeerMessageSend, SizesASenderMaySend) {
  EXPECT_FALSE(pm::Sendable(0));
  EXPECT_TRUE(pm::Sendable(1));
  EXPECT_TRUE(pm::Sendable(pm::kMaxEnvelopeBytes));
  EXPECT_FALSE(pm::Sendable(pm::kMaxEnvelopeBytes + 1));
}

TEST(PeerMessageRefusal, NamesAreWords) {
  EXPECT_STREQ(pm::RefusalName(pm::Refusal::kOversize), "oversize");
  EXPECT_STREQ(pm::RefusalName(pm::Refusal::kWrongSender), "wrong_sender");
  EXPECT_STREQ(pm::RefusalName(pm::Refusal::kUnexpectedArm), "unexpected_message");
}

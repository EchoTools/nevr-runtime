#include "runtime/server/websocket_frame.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr uint64_t kWireMagic = 0xBB8CE7A278BB40F6ULL;
constexpr size_t kHeaderSize = sizeof(uint64_t) + sizeof(EchoVR::SymbolId) + sizeof(uint64_t);

std::string Encode(EchoVR::SymbolId messageId, const std::string& payload) {
  std::string frame(kHeaderSize + payload.size(), '\0');
  const uint64_t length = payload.size();
  std::memcpy(frame.data(), &kWireMagic, sizeof(kWireMagic));
  std::memcpy(frame.data() + sizeof(kWireMagic), &messageId, sizeof(messageId));
  std::memcpy(frame.data() + sizeof(kWireMagic) + sizeof(messageId), &length, sizeof(length));
  if (!payload.empty()) std::memcpy(frame.data() + kHeaderSize, payload.data(), payload.size());
  return frame;
}

void Append(std::string& destination, const std::string& source) { destination.append(source); }

TEST(WebSocketFrame, PreservesPayloadsThatShareTheirFirstEightBytes) {
  const EchoVR::SymbolId messageId = 0x8877665544332211ULL;
  const std::string prefix = "same-8!!";
  ASSERT_EQ(prefix.size(), 8U);
  const std::string first = prefix + "-payload-a";
  const std::string second = prefix + "-payload-b";
  std::string frame = Encode(messageId, first);
  Append(frame, Encode(messageId, second));

  const auto parsed = GameServer::ParseServerDbFrame(frame, 10);
  ASSERT_EQ(parsed.status, GameServer::WebSocketFrameStatus::Complete);
  ASSERT_EQ(parsed.messages.size(), 2U);
  EXPECT_EQ(parsed.messages[0].payload, std::vector<UINT8>(first.begin(), first.end()));
  EXPECT_EQ(parsed.messages[1].payload, std::vector<UINT8>(second.begin(), second.end()));
}

TEST(WebSocketFrame, PreservesIdenticalConsecutiveMessagesAndZeroThroughSevenBytePayloads) {
  std::string frame;
  for (size_t length = 0; length <= 7; ++length) {
    const std::string payload(length, static_cast<char>('a' + length));
    Append(frame, Encode(0xAABBCCDD, payload));
    Append(frame, Encode(0xAABBCCDD, payload));
  }

  const auto parsed = GameServer::ParseServerDbFrame(frame, 20);
  ASSERT_EQ(parsed.status, GameServer::WebSocketFrameStatus::Complete);
  ASSERT_EQ(parsed.messages.size(), 16U);
  for (size_t index = 0; index < parsed.messages.size(); ++index) {
    const size_t payloadLength = index / 2;
    EXPECT_EQ(parsed.messages[index].payload.size(), payloadLength);
    EXPECT_EQ(parsed.messages[index].msgId, 0xAABBCCDDULL);
  }
}

TEST(WebSocketFrame, ParsesConcatenatedMessagesInWireOrder) {
  std::string frame;
  Append(frame, Encode(3, "third"));
  Append(frame, Encode(1, "first"));
  Append(frame, Encode(2, "second"));

  const auto parsed = GameServer::ParseServerDbFrame(frame, 10);
  ASSERT_EQ(parsed.status, GameServer::WebSocketFrameStatus::Complete);
  ASSERT_EQ(parsed.messages.size(), 3U);
  EXPECT_EQ(parsed.messages[0].msgId, 3U);
  EXPECT_EQ(parsed.messages[1].msgId, 1U);
  EXPECT_EQ(parsed.messages[2].msgId, 2U);
}

TEST(WebSocketFrame, BoundsShortTruncatedAndInvalidFrames) {
  for (size_t size = 0; size < kHeaderSize; ++size) {
    const auto parsed = GameServer::ParseServerDbFrame(std::string(size, '\0'), 10);
    EXPECT_EQ(parsed.status, GameServer::WebSocketFrameStatus::TooShort) << "size=" << size;
    EXPECT_TRUE(parsed.messages.empty());
  }

  std::string truncatedHeader = Encode(1, "data");
  truncatedHeader.resize(kHeaderSize - 1);
  EXPECT_EQ(GameServer::ParseServerDbFrame(truncatedHeader, 10).status,
            GameServer::WebSocketFrameStatus::TooShort);

  std::string truncatedPayload = Encode(2, "data");
  truncatedPayload.pop_back();
  const auto payloadResult = GameServer::ParseServerDbFrame(truncatedPayload, 10);
  EXPECT_EQ(payloadResult.status, GameServer::WebSocketFrameStatus::TruncatedPayload);
  EXPECT_EQ(payloadResult.errorMessageId, 2U);

  std::string invalidMagic(kHeaderSize, '\0');
  const auto magicResult = GameServer::ParseServerDbFrame(invalidMagic, 10);
  EXPECT_EQ(magicResult.status, GameServer::WebSocketFrameStatus::InvalidMagic);
}

TEST(WebSocketFrame, DropsOversizedPayloadWithoutLosingFollowingValidFrame) {
  std::string oversized(1024 * 1024 + 1, 'x');
  std::string frame = Encode(9, oversized);
  Append(frame, Encode(10, "valid"));

  const auto parsed = GameServer::ParseServerDbFrame(frame, 10);
  EXPECT_EQ(parsed.status, GameServer::WebSocketFrameStatus::OversizedMessage);
  ASSERT_EQ(parsed.messages.size(), 1U);
  EXPECT_EQ(parsed.messages[0].msgId, 10U);
  EXPECT_EQ(parsed.messages[0].payload, std::vector<UINT8>({'v', 'a', 'l', 'i', 'd'}));
}

TEST(WebSocketFrame, EnforcesQueueLimitWithoutReorderingAcceptedMessages) {
  std::string frame = Encode(1, "one");
  Append(frame, Encode(2, "two"));
  Append(frame, Encode(3, "three"));

  const auto parsed = GameServer::ParseServerDbFrame(frame, 2);
  EXPECT_EQ(parsed.status, GameServer::WebSocketFrameStatus::QueueLimit);
  ASSERT_EQ(parsed.messages.size(), 2U);
  EXPECT_EQ(parsed.messages[0].msgId, 1U);
  EXPECT_EQ(parsed.messages[1].msgId, 2U);
}
}  // namespace

// The lobby settings relay (#905): a frame the game service sends as SNSLobbySettingsResponse is decoded, raised as
// the game's local event with plain JSON, and sent to every connected peer. The sinks stand in for the two game
// calls (CBroadcaster::ReceiveLocalEvent, CBroadcaster::Send), so the whole sequence runs without a game process.
#include "runtime/server/lobby_settings_relay.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using nevr_game_server::BuildLobbySettingsLocalEvent;
using nevr_game_server::DecodeLobbySettings;
using nevr_game_server::FormatLobbySettingsCleared;
using nevr_game_server::kLobbySettingsAllPeers;
using nevr_game_server::kLobbySettingsMaxJsonBytes;
using nevr_game_server::LobbySettingsDrop;
using nevr_game_server::LobbySettingsLogLevel;
using nevr_game_server::LobbySettingsRecord;
using nevr_game_server::LobbySettingsSinks;
using nevr_game_server::RelayLobbySettings;

// A document shaped like the one the retail handler reads (keys from echovr.exe: parties_unlocked and
// allowed_levels in OnLobbySettings 0x1401c2a20, event_matchtype in ProcessPostMatchProfileUpdates 0x140192d90).
const char kDocument[] =
    R"({"parties_unlocked":true,"allowed_levels":["mpl_arena_a"],"event_matchtype":7,"big":18446744073709551615})";

void PutLe64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

// The wire payload: u64 LE decompressed length, then the zlib stream.
std::vector<uint8_t> MakeFrame(const std::string& json, uint64_t declaredLength) {
  std::vector<uint8_t> frame;
  PutLe64(frame, declaredLength);
  uLongf packedSize = compressBound(static_cast<uLong>(json.size()));
  std::vector<uint8_t> packed(packedSize);
  const int rc = compress(packed.data(), &packedSize, reinterpret_cast<const Bytef*>(json.data()),
                          static_cast<uLong>(json.size()));
  EXPECT_EQ(rc, Z_OK);
  frame.insert(frame.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packedSize));
  return frame;
}

std::vector<uint8_t> MakeFrame(const std::string& json) { return MakeFrame(json, json.size()); }

struct Calls {
  std::vector<std::string> order;
  std::vector<uint8_t> localMessage;
  std::vector<uint8_t> sendItem;
  std::vector<uint8_t> sendBody;
  uint64_t sendPeer = 0;
  int sends = 0;
  int locals = 0;
};

LobbySettingsSinks MakeSinks(Calls& calls, bool broadcasterPresent = true, int32_t sendResult = 0) {
  LobbySettingsSinks sinks;
  sinks.raiseLocalEvent = [&calls, broadcasterPresent](void* message, uint64_t size) {
    calls.order.push_back("local");
    ++calls.locals;
    const auto* bytes = static_cast<const uint8_t*>(message);
    calls.localMessage.assign(bytes, bytes + size);
    return broadcasterPresent;
  };
  sinks.sendToPeer = [&calls, sendResult](const void* item, uint64_t itemSize, const void* body, uint64_t bodySize,
                                          uint64_t peer) {
    calls.order.push_back("send");
    ++calls.sends;
    const auto* itemBytes = static_cast<const uint8_t*>(item);
    const auto* bodyBytes = static_cast<const uint8_t*>(body);
    calls.sendItem.assign(itemBytes, itemBytes + itemSize);
    calls.sendBody.assign(bodyBytes, bodyBytes + bodySize);
    calls.sendPeer = peer;
    return sendResult;
  };
  return sinks;
}

}  // namespace

TEST(LobbySettingsDecode, RoundTripsTheDocumentByteForByte) {
  const std::string json = kDocument;
  const auto frame = MakeFrame(json);
  const auto decoded = DecodeLobbySettings(frame.data(), frame.size());
  EXPECT_EQ(decoded.drop, LobbySettingsDrop::kNone);
  EXPECT_EQ(decoded.json, json);
}

TEST(LobbySettingsDecode, FrameWithoutALengthFieldIsTooShort) {
  const std::vector<uint8_t> seven(7, 0);
  EXPECT_EQ(DecodeLobbySettings(seven.data(), seven.size()).drop, LobbySettingsDrop::kTooShort);
  EXPECT_EQ(DecodeLobbySettings(nullptr, 0).drop, LobbySettingsDrop::kTooShort);
  EXPECT_EQ(DecodeLobbySettings(nullptr, 64).drop, LobbySettingsDrop::kTooShort);
}

TEST(LobbySettingsDecode, ZeroAndOversizeLengthsAreRefusedBeforeAnyAllocation) {
  const std::string json = kDocument;
  auto zero = MakeFrame(json, 0);
  EXPECT_EQ(DecodeLobbySettings(zero.data(), zero.size()).drop, LobbySettingsDrop::kBadLength);

  auto atLimit = MakeFrame(json, kLobbySettingsMaxJsonBytes);
  EXPECT_NE(DecodeLobbySettings(atLimit.data(), atLimit.size()).drop, LobbySettingsDrop::kBadLength);

  auto overLimit = MakeFrame(json, kLobbySettingsMaxJsonBytes + 1);
  EXPECT_EQ(DecodeLobbySettings(overLimit.data(), overLimit.size()).drop, LobbySettingsDrop::kBadLength);

  auto huge = MakeFrame(json, 0x8000000000000000ULL);
  EXPECT_EQ(DecodeLobbySettings(huge.data(), huge.size()).drop, LobbySettingsDrop::kBadLength);
}

TEST(LobbySettingsDecode, AFrameLargerThanZlibsLengthTypeIsRefusedBeforeInflating) {
  // zlib's uLong is 32 bits on the Windows targets; the size is only compared, so no 4 GiB buffer is built.
  const auto frame = MakeFrame(kDocument);
  EXPECT_EQ(DecodeLobbySettings(frame.data(), 0xFFFFFFFFULL + 9ULL).drop, LobbySettingsDrop::kBadLength);
}

TEST(LobbySettingsDecode, CorruptStreamIsRefusedWithTheZlibCode) {
  auto frame = MakeFrame(kDocument);
  for (size_t i = 8; i < frame.size(); ++i) frame[i] = 0xA5;
  const auto decoded = DecodeLobbySettings(frame.data(), frame.size());
  EXPECT_EQ(decoded.drop, LobbySettingsDrop::kInflateFailed);
  EXPECT_NE(decoded.zlibResult, Z_OK);
  EXPECT_TRUE(decoded.json.empty());
}

TEST(LobbySettingsDecode, DeclaredLengthBelowTheRealSizeIsRefused) {
  const std::string json = kDocument;
  auto frame = MakeFrame(json, json.size() - 1);
  EXPECT_EQ(DecodeLobbySettings(frame.data(), frame.size()).drop, LobbySettingsDrop::kInflateFailed);
}

TEST(LobbySettingsDecode, DeclaredLengthAboveTheRealSizeUsesTheInflatedLength) {
  // The retail handler passes the declared length in and parses what Decompress produced (0x14017dee0).
  const std::string json = kDocument;
  auto frame = MakeFrame(json, json.size() + 100);
  const auto decoded = DecodeLobbySettings(frame.data(), frame.size());
  EXPECT_EQ(decoded.drop, LobbySettingsDrop::kNone);
  EXPECT_EQ(decoded.json, json);
}

TEST(LobbySettingsDecode, NonJsonAndNonObjectAreRefused) {
  const std::string notJson = "this is not json";
  auto a = MakeFrame(notJson);
  EXPECT_EQ(DecodeLobbySettings(a.data(), a.size()).drop, LobbySettingsDrop::kNotJson);

  const std::string array = "[1,2,3]";
  auto b = MakeFrame(array);
  EXPECT_EQ(DecodeLobbySettings(b.data(), b.size()).drop, LobbySettingsDrop::kNotObject);
}

TEST(LobbySettingsLocalEvent, IsEightZeroBytesThenThePlainJsonWithNoTerminator) {
  const std::string json = kDocument;
  const auto message = BuildLobbySettingsLocalEvent(json);
  ASSERT_EQ(message.size(), 8 + json.size());
  for (size_t i = 0; i < 8; ++i) EXPECT_EQ(message[i], 0) << "item byte " << i;
  EXPECT_EQ(std::string(message.begin() + 8, message.end()), json);
}

TEST(LobbySettingsRelay, RaisesTheLocalEventThenSendsToAllPeers) {
  const std::string json = kDocument;
  const auto frame = MakeFrame(json);
  Calls calls;
  const auto report = RelayLobbySettings(frame.data(), frame.size(), "sess-1", MakeSinks(calls, true, 0));

  EXPECT_TRUE(report.relayed);
  EXPECT_EQ(report.drop, LobbySettingsDrop::kNone);
  ASSERT_EQ(calls.order, (std::vector<std::string>{"local", "send"}));

  EXPECT_EQ(calls.localMessage, BuildLobbySettingsLocalEvent(json));

  EXPECT_EQ(calls.sendItem, std::vector<uint8_t>(8, 0));
  EXPECT_EQ(std::string(calls.sendBody.begin(), calls.sendBody.end()), json);
  EXPECT_EQ(calls.sendPeer, kLobbySettingsAllPeers);
  EXPECT_EQ(calls.sendPeer, 0xFFFFFFFFFFFFFFFDULL);
}

TEST(LobbySettingsRelay, ReportCarriesSessionSizesAndTheSendResult) {
  const std::string json = kDocument;
  const auto frame = MakeFrame(json);
  Calls calls;
  const auto report = RelayLobbySettings(frame.data(), frame.size(), "sess-42", MakeSinks(calls, true, 5));

  EXPECT_EQ(report.level, LobbySettingsLogLevel::kInfo);
  EXPECT_EQ(report.wireBytes, frame.size());
  EXPECT_EQ(report.jsonBytes, json.size());
  EXPECT_EQ(report.sendResult, 5);
  EXPECT_NE(report.line.find("[NEVR.GAMESERVER] lobby settings relayed"), std::string::npos) << report.line;
  EXPECT_NE(report.line.find("session_id=sess-42"), std::string::npos) << report.line;
  EXPECT_NE(report.line.find("wire_bytes=" + std::to_string(frame.size())), std::string::npos) << report.line;
  EXPECT_NE(report.line.find("json_bytes=" + std::to_string(json.size())), std::string::npos) << report.line;
  EXPECT_NE(report.line.find("send_rc=5"), std::string::npos) << report.line;
}

TEST(LobbySettingsRelay, ALongSessionIdIsCutToAUuidPlusSlack) {
  const auto frame = MakeFrame(kDocument);
  Calls calls;
  const std::string longId(200, 'x');
  const auto report = RelayLobbySettings(frame.data(), frame.size(), longId, MakeSinks(calls));
  EXPECT_NE(report.line.find("session_id=" + std::string(64, 'x') + " "), std::string::npos) << report.line;
  EXPECT_EQ(report.line.find(std::string(65, 'x')), std::string::npos) << report.line;
}

TEST(LobbySettingsRelay, NoSessionIsNamedNotLeftBlank) {
  const auto frame = MakeFrame(kDocument);
  Calls calls;
  const auto report = RelayLobbySettings(frame.data(), frame.size(), "", MakeSinks(calls));
  EXPECT_NE(report.line.find("session_id=none"), std::string::npos) << report.line;
}

TEST(LobbySettingsRelay, TheLineNeverCarriesTheDocumentText) {
  // docs/standards/logging.md Rule 4: remote text is not a log field.
  const std::string json = R"({"marker":"DOCUMENT-TEXT-MARKER"})";
  const auto frame = MakeFrame(json);
  Calls calls;
  const auto relayed = RelayLobbySettings(frame.data(), frame.size(), "s", MakeSinks(calls));
  EXPECT_EQ(relayed.line.find("DOCUMENT-TEXT-MARKER"), std::string::npos) << relayed.line;

  const std::string bad = "DOCUMENT-TEXT-MARKER not json";
  const auto badFrame = MakeFrame(bad);
  Calls none;
  const auto dropped = RelayLobbySettings(badFrame.data(), badFrame.size(), "s", MakeSinks(none));
  EXPECT_EQ(dropped.line.find("DOCUMENT-TEXT-MARKER"), std::string::npos) << dropped.line;
}

TEST(LobbySettingsRelay, ABadFrameCallsNothingAndWarns) {
  const std::string bad = "not json at all";
  const auto frame = MakeFrame(bad);
  Calls calls;
  const auto report = RelayLobbySettings(frame.data(), frame.size(), "sess-9", MakeSinks(calls));

  EXPECT_FALSE(report.relayed);
  EXPECT_EQ(report.drop, LobbySettingsDrop::kNotJson);
  EXPECT_EQ(report.level, LobbySettingsLogLevel::kWarning);
  EXPECT_EQ(calls.locals, 0);
  EXPECT_EQ(calls.sends, 0);
  EXPECT_NE(report.line.find("lobby settings dropped"), std::string::npos) << report.line;
  EXPECT_NE(report.line.find("reason=not_json"), std::string::npos) << report.line;
  EXPECT_NE(report.line.find("session_id=sess-9"), std::string::npos) << report.line;
  EXPECT_NE(report.line.find("wire_bytes=" + std::to_string(frame.size())), std::string::npos) << report.line;
}

TEST(LobbySettingsRelay, AnInflateFailureNamesTheZlibCode) {
  auto frame = MakeFrame(kDocument);
  for (size_t i = 8; i < frame.size(); ++i) frame[i] = 0xA5;
  Calls calls;
  const auto report = RelayLobbySettings(frame.data(), frame.size(), "s", MakeSinks(calls));
  EXPECT_EQ(report.drop, LobbySettingsDrop::kInflateFailed);
  EXPECT_NE(report.line.find("reason=inflate_failed"), std::string::npos) << report.line;
  EXPECT_NE(report.line.find("zlib_rc=" + std::to_string(Z_DATA_ERROR)), std::string::npos) << report.line;
}

TEST(LobbySettingsRelay, NoBroadcasterStopsBeforeTheSend) {
  const auto frame = MakeFrame(kDocument);
  Calls calls;
  const auto report = RelayLobbySettings(frame.data(), frame.size(), "s", MakeSinks(calls, /*broadcasterPresent=*/false));

  EXPECT_FALSE(report.relayed);
  EXPECT_EQ(report.drop, LobbySettingsDrop::kNoBroadcaster);
  EXPECT_EQ(report.level, LobbySettingsLogLevel::kWarning);
  EXPECT_EQ(calls.locals, 1);
  EXPECT_EQ(calls.sends, 0);
  EXPECT_NE(report.line.find("reason=no_broadcaster"), std::string::npos) << report.line;
}

TEST(LobbySettingsRelay, MissingSinksAreNamedAsNoBroadcasterNotAsAnException) {
  // gameserver_callbacks.cpp leaves both sinks empty when the game has no broadcaster yet.
  const auto frame = MakeFrame(kDocument);
  const auto report = RelayLobbySettings(frame.data(), frame.size(), "s", LobbySettingsSinks{});
  EXPECT_FALSE(report.relayed);
  EXPECT_EQ(report.drop, LobbySettingsDrop::kNoBroadcaster);
  EXPECT_NE(report.line.find("reason=no_broadcaster"), std::string::npos) << report.line;
}

TEST(LobbySettingsRelay, ASinkThatThrowsIsContained) {
  const auto frame = MakeFrame(kDocument);
  Calls calls;
  auto sinks = MakeSinks(calls);
  sinks.raiseLocalEvent = [](void*, uint64_t) -> bool { throw std::runtime_error("sink failed"); };
  const auto report = RelayLobbySettings(frame.data(), frame.size(), "s", sinks);

  EXPECT_FALSE(report.relayed);
  EXPECT_EQ(report.drop, LobbySettingsDrop::kException);
  EXPECT_EQ(report.level, LobbySettingsLogLevel::kWarning);
  EXPECT_EQ(calls.sends, 0);
  EXPECT_NE(report.line.find("reason=exception"), std::string::npos) << report.line;
}

TEST(LobbySettingsRelay, EachFrameIsRelayedIndependently) {
  // Settings can change within a session; a second document replaces the first and is sent again.
  const auto first = MakeFrame(R"({"a":1})");
  const auto second = MakeFrame(R"({"a":2})");
  Calls calls;
  const auto sinks = MakeSinks(calls);
  EXPECT_TRUE(RelayLobbySettings(first.data(), first.size(), "s", sinks).relayed);
  EXPECT_TRUE(RelayLobbySettings(second.data(), second.size(), "s", sinks).relayed);
  EXPECT_EQ(calls.locals, 2);
  EXPECT_EQ(calls.sends, 2);
  EXPECT_EQ(std::string(calls.sendBody.begin(), calls.sendBody.end()), R"({"a":2})");
}

TEST(LobbySettingsCleared, LineNamesReasonSessionAndWhetherSettingsHadArrived) {
  LobbySettingsRecord present;
  present.present = true;
  present.wireBytes = 123;
  present.jsonBytes = 456;
  present.sessionId = "sess-7";
  const std::string withSettings = FormatLobbySettingsCleared(present, "session_end", "sess-7");
  EXPECT_NE(withSettings.find("[NEVR.GAMESERVER] lobby settings record cleared"), std::string::npos) << withSettings;
  EXPECT_NE(withSettings.find("reason=session_end"), std::string::npos) << withSettings;
  EXPECT_NE(withSettings.find("session_id=sess-7"), std::string::npos) << withSettings;
  EXPECT_NE(withSettings.find("had_settings=1"), std::string::npos) << withSettings;
  EXPECT_NE(withSettings.find("json_bytes=456"), std::string::npos) << withSettings;

  const std::string without = FormatLobbySettingsCleared(LobbySettingsRecord{}, "session_start", "");
  EXPECT_NE(without.find("reason=session_start"), std::string::npos) << without;
  EXPECT_NE(without.find("session_id=none"), std::string::npos) << without;
  EXPECT_NE(without.find("had_settings=0"), std::string::npos) << without;
  EXPECT_NE(without.find("json_bytes=0"), std::string::npos) << without;
}

// Tests for the self-check unit (compat/self_check.{h,cpp}) and the SNSRemoteLogSetv3 frame it sends
// (nevr_evr_codec::BuildRemoteLogSet). The frame is read back by the small decoder below, which
// re-implements the game service's reader (nakama server/evr/login_remotelogset.go `Stream` and
// core_stream.go `StreamStringTable`) and shares no code with the codec.

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/self_check.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

struct Decoded {
  bool ok = false;
  uint64_t symbol = 0;
  uint64_t platform = 0;
  uint64_t account = 0;
  uint64_t level = 0;
  std::vector<std::string> logs;
};

uint64_t U64(const std::string& s, std::size_t at) {
  uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return v;
}
uint32_t U32(const std::string& s, std::size_t at) {
  uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return v;
}

// Mirrors nakama's RemoteLogSet.Stream: EvrId{platform u64, account u64}, Unk0..Unk3 (4 x u64), LogLevel u64,
// then StreamStringTable: count u64, offsets u32 for entries 1..n-1 (entry 0 is at offset 0), the strings
// NUL terminated from the end of the offsets.
Decoded DecodeNakamaStyle(const std::string& frame) {
  Decoded d;
  if (frame.size() < 24) return d;
  if (frame.substr(0, 8) != std::string("\xf6\x40\xbb\x78\xa2\xe7\x8c\xbb", 8)) return d;
  d.symbol = U64(frame, 8);
  const uint64_t length = U64(frame, 16);
  if (frame.size() != 24 + length) return d;
  const std::size_t base = 24;
  if (length < 8 * 7 + 8) return d;
  d.platform = U64(frame, base + 0);
  d.account = U64(frame, base + 8);
  d.level = U64(frame, base + 8 * 6);
  std::size_t at = base + 8 * 7;
  const uint64_t count = U64(frame, at);
  at += 8;
  if (count > 1024) return d;
  std::vector<uint32_t> offsets(static_cast<std::size_t>(count), 0);
  for (std::size_t i = 1; i < count; ++i) {
    offsets[i] = U32(frame, at);
    at += 4;
  }
  const std::size_t start = at;
  for (std::size_t i = 0; i < count; ++i) {
    std::size_t p = start + offsets[i];
    std::string s;
    while (p < frame.size() && frame[p] != '\0') s.push_back(frame[p++]);
    if (p >= frame.size()) return d;  // no terminator
    d.logs.push_back(std::move(s));
  }
  d.ok = true;
  return d;
}

}  // namespace

TEST(RemoteLogFrame, OneStringDecodesLikeTheGameService) {
  nevr_evr_codec::UserId user;
  user.platformCode = 4;
  user.accountId = 1234567890123ULL;
  const std::string frame = nevr_evr_codec::BuildRemoteLogSet(user, 2, {"hello"});
  const Decoded d = DecodeNakamaStyle(frame);
  ASSERT_TRUE(d.ok);
  EXPECT_EQ(d.symbol, nevr_evr_codec::kSymRemoteLogSet);
  EXPECT_EQ(d.platform, 4u);
  EXPECT_EQ(d.account, 1234567890123ULL);
  EXPECT_EQ(d.level, 2u);
  ASSERT_EQ(d.logs.size(), 1u);
  EXPECT_EQ(d.logs[0], "hello");
}

TEST(RemoteLogFrame, ManyStringsKeepOrderAndOffsets) {
  nevr_evr_codec::UserId user;
  user.platformCode = 2;
  user.accountId = 7;
  std::vector<std::string> in;
  for (int i = 0; i < 16; ++i) in.push_back(std::string(static_cast<std::size_t>(i), 'a' + i % 26) + "#" + std::to_string(i));
  in.push_back("");  // an empty string is a legal entry
  const std::string frame = nevr_evr_codec::BuildRemoteLogSet(user, 2, in);
  const Decoded d = DecodeNakamaStyle(frame);
  ASSERT_TRUE(d.ok);
  EXPECT_EQ(d.logs, in);
}

TEST(RemoteLogFrame, FieldLayoutMatchesTheGameHeader) {
  nevr_evr_codec::UserId user;
  user.platformCode = 4;
  user.accountId = 9;
  const std::string frame = nevr_evr_codec::BuildRemoteLogSet(user, 8, {"x", "yy"});
  // payload: EvrId(16) + 4 u64 zero (session uuid + 16 text bytes) + level u64, then the table:
  // u32 count, u32 offsets[count] with offsets[0] == 0, strings (the game's own table layout).
  const std::size_t base = 24;
  for (std::size_t i = 16; i < 48; ++i) EXPECT_EQ(frame.at(base + i), '\0') << "byte " << i;
  EXPECT_EQ(U64(frame, base + 48), 8u);
  EXPECT_EQ(U32(frame, base + 56), 2u);   // count (u32)
  EXPECT_EQ(U32(frame, base + 60), 0u);   // offsets[0]
  EXPECT_EQ(U32(frame, base + 64), 2u);   // offsets[1] = len("x") + NUL
  EXPECT_EQ(frame.substr(base + 68), std::string("x\0yy\0", 5));
}

namespace {

struct Sent {
  std::vector<std::string> frames;
  bool accept = true;
};
Sent g_sent;
std::vector<nevr_self_check::LogRecord> g_logged;

bool Sender(const std::string& frame) {
  if (!g_sent.accept) return false;
  g_sent.frames.push_back(frame);
  return true;
}
void Logger(const nevr_self_check::LogRecord& r) { g_logged.push_back(r); }

class SelfCheck : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_self_check::ResetForTest();
    g_sent = Sent{};
    g_logged.clear();
    nevr_self_check::SetEnabled(true);
    nevr_self_check::SetSender(&Sender);
    nevr_self_check::SetLogSink(&Logger);
    nevr_self_check::SetBuild("4.0.0-rc.1+abc");
  }
  static std::vector<nlohmann::json> AllSent() {
    std::vector<nlohmann::json> out;
    for (const std::string& f : g_sent.frames) {
      const Decoded d = DecodeNakamaStyle(f);
      EXPECT_TRUE(d.ok);
      for (const std::string& s : d.logs) out.push_back(nlohmann::json::parse(s));
    }
    return out;
  }
};

}  // namespace

TEST_F(SelfCheck, ReportBeforeLoginIsLoggedNowAndSentAfterLogin) {
  const auto id = nevr_self_check::Register({"party_data_share", "PartyUpdateSuccess", nullptr});
  nevr_self_check::Report(id, "PartyUpdateFailure", false);
  ASSERT_EQ(g_logged.size(), 1u);  // the nevr log line is written when it happens
  EXPECT_EQ(g_logged[0].name, "party_data_share");
  EXPECT_FALSE(g_logged[0].pass);
  EXPECT_EQ(g_logged[0].expected, "PartyUpdateSuccess");
  EXPECT_EQ(g_logged[0].observed, "PartyUpdateFailure");

  nevr_self_check::Flush();
  EXPECT_TRUE(g_sent.frames.empty()) << "nothing may be sent before LoginSuccess";

  nevr_evr_codec::UserId user;
  user.platformCode = 4;
  user.accountId = 42;
  nevr_self_check::SetLoggedIn(true, user);
  nevr_self_check::Flush();
  ASSERT_EQ(g_sent.frames.size(), 1u);
  const auto results = AllSent();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]["message"], "nevr_self_check");
  EXPECT_EQ(results[0]["message_type"], "NEVR_SELF_CHECK");
  EXPECT_EQ(results[0]["check"], "party_data_share");
  EXPECT_EQ(results[0]["pass"], false);
  EXPECT_EQ(results[0]["expected"], "PartyUpdateSuccess");
  EXPECT_EQ(results[0]["observed"], "PartyUpdateFailure");
  EXPECT_EQ(results[0]["build"], "4.0.0-rc.1+abc");
  EXPECT_EQ(results[0]["userid"], "OVR-ORG-42");
  EXPECT_EQ(results[0]["seq"], 1);
  EXPECT_EQ(DecodeNakamaStyle(g_sent.frames[0]).account, 42u);
}

TEST_F(SelfCheck, OnlyTheLoginConnectionOfAClientBuildWithTheUnitOnAsksForDebug) {
  EXPECT_TRUE(nevr_self_check::WantsRemoteDebug(1, /*isServer=*/false));
  EXPECT_FALSE(nevr_self_check::WantsRemoteDebug(0, false)) << "the config connection";
  EXPECT_FALSE(nevr_self_check::WantsRemoteDebug(2, false)) << "the matchmaker connection";
  EXPECT_FALSE(nevr_self_check::WantsRemoteDebug(1, /*isServer=*/true)) << "a dedicated game server";
  nevr_self_check::SetEnabled(false);
  EXPECT_FALSE(nevr_self_check::WantsRemoteDebug(1, false)) << "the unit is off";
}

TEST_F(SelfCheck, DisabledUnitRecordsNothing) {
  nevr_self_check::SetEnabled(false);
  const auto id = nevr_self_check::Register({"x", "y", nullptr});
  nevr_self_check::Report(id, "z", true);
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 1});
  nevr_self_check::Flush();
  EXPECT_TRUE(g_logged.empty());
  EXPECT_TRUE(g_sent.frames.empty());
}

TEST_F(SelfCheck, ResultsFromManyChecksShareOneFrameUpToTheBatchLimit) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 1});
  std::vector<nevr_self_check::CheckId> ids;
  for (int i = 0; i < 20; ++i) ids.push_back(nevr_self_check::Register({("c" + std::to_string(i)).c_str(), "ok", nullptr}));
  for (auto id : ids) nevr_self_check::Report(id, "ok", true);
  nevr_self_check::Flush();
  ASSERT_EQ(g_sent.frames.size(), 2u);
  EXPECT_EQ(DecodeNakamaStyle(g_sent.frames[0]).logs.size(), nevr_self_check::kMaxStringsPerFrame);
  EXPECT_EQ(DecodeNakamaStyle(g_sent.frames[1]).logs.size(), 20u - nevr_self_check::kMaxStringsPerFrame);
}

TEST_F(SelfCheck, AFailedSendKeepsTheResultForTheNextFlush) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 1});
  const auto id = nevr_self_check::Register({"a", "ok", nullptr});
  g_sent.accept = false;
  nevr_self_check::Report(id, "ok", true);
  nevr_self_check::Flush();
  EXPECT_TRUE(g_sent.frames.empty());
  g_sent.accept = true;
  nevr_self_check::Flush();
  EXPECT_EQ(g_sent.frames.size(), 1u);
  nevr_self_check::Flush();
  EXPECT_EQ(g_sent.frames.size(), 1u) << "a delivered result is not sent twice";
}

TEST_F(SelfCheck, PerCheckCapEmitsOneCappedResultThenCountsSilently) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 1});
  const auto id = nevr_self_check::Register({"chatty", "ok", nullptr});
  for (int i = 0; i < 40; ++i) nevr_self_check::Report(id, "ok", true);
  nevr_self_check::Flush();
  const auto results = AllSent();
  ASSERT_EQ(results.size(), nevr_self_check::kMaxResultsPerCheck + 1u);
  EXPECT_EQ(results.back()["observed"], "capped");
  EXPECT_EQ(results.back()["suppressed_after"], static_cast<int>(nevr_self_check::kMaxResultsPerCheck));
}

TEST_F(SelfCheck, PreLoginQueueIsBoundedAndReportsWhatItDropped) {
  // More distinct checks than the queue holds, so the per-check cap does not hide the queue bound.
  std::vector<nevr_self_check::CheckId> ids;
  for (std::size_t i = 0; i < nevr_self_check::kMaxChecks; ++i) {
    ids.push_back(nevr_self_check::Register({("e" + std::to_string(i)).c_str(), "ok", nullptr}));
  }
  // Report each of the kMaxChecks checks once, then again: 2 x 64 results into a 64-entry queue.
  for (int round = 0; round < 2; ++round) {
    for (auto id : ids) nevr_self_check::Report(id, "ok", true);
  }
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 1});
  nevr_self_check::Flush();
  const auto results = AllSent();
  EXPECT_LE(results.size(), nevr_self_check::kMaxQueued + 1u);
  bool sawDropped = false;
  for (const auto& r : results) sawDropped = sawDropped || (r.contains("dropped") && r["dropped"].get<int>() > 0);
  EXPECT_TRUE(sawDropped);
}

namespace {
int g_probeCalls = 0;
bool CountingProbe(nevr_self_check::Observation* out) {
  if (g_probeCalls++ == 0) return false;  // nothing to say yet
  out->observed = "loads=2 patches=2";
  out->pass = true;
  return true;
}
}  // namespace

TEST_F(SelfCheck, AProbeRunsOnFlushAndReportsWhenItHasAResult) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 1});
  g_probeCalls = 0;
  nevr_self_check::Register({"mm_reload", "patches == loads", &CountingProbe});
  nevr_self_check::Flush();
  EXPECT_TRUE(g_sent.frames.empty());
  nevr_self_check::Flush();
  const auto results = AllSent();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]["check"], "mm_reload");
  EXPECT_EQ(results[0]["observed"], "loads=2 patches=2");
  EXPECT_EQ(results[0]["pass"], true);
}

TEST_F(SelfCheck, LongTextIsTruncatedAndStillValidJson) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 1});
  const auto id = nevr_self_check::Register({"long", "ok", nullptr});
  const std::string big(5000, 'q');
  nevr_self_check::Report(id, big + "\"quoted\\\n", true);
  nevr_self_check::Flush();
  const auto results = AllSent();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_LE(results[0]["observed"].get<std::string>().size(), nevr_self_check::kMaxTextBytes);
}

TEST_F(SelfCheck, InvalidUtf8InTextCannotBreakTheRecord) {
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 1});
  const auto id = nevr_self_check::Register({"bytes", "ok", nullptr});
  nevr_self_check::Report(id, std::string("bad\xff\xfe" "tail"), true);
  nevr_self_check::Flush();
  const auto results = AllSent();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]["check"], "bytes");
}

// Peer timeout logging: the server's communication-timeout line and the client's in-game send state and no-send
// warning (runtime/patch/peer_timeout_log_rules.h). Pure: every time is passed in; no sleep, no clock.
//
// Mutants (one line each, run against peer_timeout_log_rules.h):
//   M1 FormatPeerRemoved: gap = now - last (clamp dropped)            -> ClampsAClockThatReadsEarlier fails.
//   M2 FormatPeerRemoved: gap_s printed at half the gap                -> ServerLineNamesPeerSlotLastReceiveAndGap fails.
//   M8 LevelForRemovalReason: "communication timeout" at Info          -> TimeoutsAreWarnings fails.
//   M3 RecordSend: sends_ not incremented                              -> SendStateCountsSends fails.
//   M4 CheckNoSend: `>=` changed to `>`                                -> WarnsExactlyAtFiveSeconds fails.
//   M5 CheckNoSend: warned_ never set                                  -> WarnsOncePerSilence fails.
//   M6 RecordSend: warned_ not cleared                                 -> RearmsAfterASend fails.
//   M7 CheckNoSend: returns a value when not in game                   -> NotInGameNeverWarns fails.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "runtime/patch/peer_timeout_log_rules.h"

namespace {

using nevr_peer_timeout_log::PeerRemoval;
using nevr_peer_timeout_log::FormatPeerRemoved;
using nevr_peer_timeout_log::LevelForRemovalReason;
using nevr_peer_timeout_log::RecognizedRemovalReason;
using nevr_peer_timeout_log::RemovalLevel;
using nevr_peer_timeout_log::FormatInGameSendState;
using nevr_peer_timeout_log::FormatNoSendWarning;
using nevr_peer_timeout_log::SendTracker;
using nevr_peer_timeout_log::kNoSendWarnMicros;

constexpr std::uint64_t kSecond = 1'000'000ULL;

// Stamps are microseconds on one clock. The gap of the case below is 15.033 s, the size of a communication timeout
// (15 s) plus its polling offset; the numbers are fixtures, not measurements of a run.
constexpr std::uint64_t kLastReceive = 37'029'000ULL;
constexpr std::uint64_t kRemoval = 52'062'000ULL;
constexpr std::uint64_t kSendsInRun = 30ULL;
constexpr std::uint64_t kEarlyClock = 100ULL;
constexpr std::uint64_t kLateClock = 900ULL;

TEST(PeerTimeoutServer, ServerLineNamesPeerSlotLastReceiveAndGap) {
    const PeerRemoval record{0, "communication timeout", "10.0.8.50:6794", kLastReceive, kRemoval};
    const std::string line = FormatPeerRemoved(record);
    EXPECT_EQ(line.rfind("[NEVR.GAMESERVER] ", 0), 0u) << line;
    EXPECT_NE(line.find("peer removed reason=communication timeout"), std::string::npos) << line;
    EXPECT_NE(line.find("slot=0"), std::string::npos) << line;
    EXPECT_NE(line.find("address=10.0.8.50:6794"), std::string::npos) << line;
    EXPECT_NE(line.find("last_receive_us=37029000"), std::string::npos) << line;
    EXPECT_NE(line.find("gap_s=15.033"), std::string::npos) << line;
    EXPECT_EQ(line.find('\n'), std::string::npos) << "a record is one line";
}

TEST(PeerTimeoutServer, ClampsAClockThatReadsEarlier) {
    const PeerRemoval record{3, "communication timeout", "10.0.0.1:1", kLateClock, kEarlyClock};
    const std::string line = FormatPeerRemoved(record);
    EXPECT_NE(line.find("gap_s=0.000"), std::string::npos) << line;
}

TEST(PeerTimeoutRemoval, TimeoutsAreWarnings) {
    EXPECT_EQ(LevelForRemovalReason("communication timeout"), RemovalLevel::kWarning);
    EXPECT_EQ(LevelForRemovalReason("connect timeout"), RemovalLevel::kWarning);
    EXPECT_EQ(LevelForRemovalReason("disconnect timeout"), RemovalLevel::kWarning);
    EXPECT_EQ(LevelForRemovalReason("no connect received"), RemovalLevel::kWarning);
}

TEST(PeerTimeoutRemoval, DisconnectsAreInfo) {
    EXPECT_EQ(LevelForRemovalReason("disconnect received"), RemovalLevel::kInfo);
    EXPECT_EQ(LevelForRemovalReason("disconnect acknowledged"), RemovalLevel::kInfo);
    EXPECT_EQ(LevelForRemovalReason("disconnected"), RemovalLevel::kInfo);
}

TEST(PeerTimeoutRemoval, AnUnknownReasonIsAWarningAndSaysSo) {
    EXPECT_EQ(LevelForRemovalReason("something new"), RemovalLevel::kWarning);
    EXPECT_FALSE(RecognizedRemovalReason("something new"));
    const PeerRemoval record{4, "something new", "10.0.0.4:4", kLateClock, kLateClock};
    const std::string line = FormatPeerRemoved(record);
    EXPECT_NE(line.find("reason=something new"), std::string::npos) << line;
    EXPECT_NE(line.find("reason_recognized=no"), std::string::npos) << line;
}

TEST(PeerTimeoutRemoval, AKnownReasonIsNotSaidToBeUnrecognized) {
    EXPECT_TRUE(RecognizedRemovalReason("communication timeout"));
    const PeerRemoval record{5, "communication timeout", "10.0.0.5:5", kLateClock, kLateClock + kSecond};
    EXPECT_EQ(FormatPeerRemoved(record).find("reason_recognized"), std::string::npos);
}

TEST(PeerTimeoutRemoval, TheReasonIsKeptInTheLine) {
    const PeerRemoval record{2, "disconnect received", "10.0.0.2:2", kLateClock, kLateClock + kSecond};
    const std::string line = FormatPeerRemoved(record);
    EXPECT_NE(line.find("reason=disconnect received"), std::string::npos) << line;
    EXPECT_NE(line.find("gap_s=1.000"), std::string::npos) << line;
}

TEST(PeerTimeoutClient, NoSendsReadsNone) {
    SendTracker tracker;
    tracker.EnterInGame(10 * kSecond);
    EXPECT_EQ(FormatInGameSendState(tracker), "[NEVR.WS] in game send_state last_send_us=none sends=0");
}

TEST(PeerTimeoutClient, SendStateCountsSendsAndKeepsTheLastTime) {
    SendTracker tracker;
    tracker.EnterInGame(10 * kSecond);
    tracker.RecordSend(11 * kSecond);
    tracker.RecordSend(12 * kSecond);
    EXPECT_EQ(tracker.Sends(), 2u);
    EXPECT_EQ(FormatInGameSendState(tracker), "[NEVR.WS] in game send_state last_send_us=12000000 sends=2");
}

TEST(PeerTimeoutClient, NotInGameNeverWarns) {
    SendTracker tracker;
    EXPECT_FALSE(tracker.CheckNoSend(100 * kSecond).has_value());
    tracker.EnterInGame(0);
    tracker.LeaveInGame();
    EXPECT_FALSE(tracker.CheckNoSend(100 * kSecond).has_value());
}

TEST(PeerTimeoutClient, DoesNotWarnBeforeFiveSeconds) {
    SendTracker tracker;
    tracker.EnterInGame(0);
    EXPECT_FALSE(tracker.CheckNoSend(kNoSendWarnMicros - 1).has_value());
}

TEST(PeerTimeoutClient, WarnsExactlyAtFiveSeconds) {
    SendTracker tracker;
    tracker.EnterInGame(0);
    const auto silent = tracker.CheckNoSend(kNoSendWarnMicros);
    ASSERT_TRUE(silent.has_value());
    EXPECT_EQ(*silent, kNoSendWarnMicros);
    EXPECT_NE(FormatNoSendWarning(tracker, *silent).find("no send to host for 5.000 s"), std::string::npos);
}

TEST(PeerTimeoutClient, WarnsOncePerSilence) {
    SendTracker tracker;
    tracker.EnterInGame(0);
    EXPECT_TRUE(tracker.CheckNoSend(6 * kSecond).has_value());
    EXPECT_FALSE(tracker.CheckNoSend(7 * kSecond).has_value());
    EXPECT_FALSE(tracker.CheckNoSend(30 * kSecond).has_value());
}

TEST(PeerTimeoutClient, RearmsAfterASend) {
    SendTracker tracker;
    tracker.EnterInGame(0);
    EXPECT_TRUE(tracker.CheckNoSend(6 * kSecond).has_value());
    tracker.RecordSend(7 * kSecond);
    EXPECT_FALSE(tracker.CheckNoSend(11 * kSecond).has_value());
    EXPECT_TRUE(tracker.CheckNoSend(12 * kSecond).has_value());
}

TEST(PeerTimeoutClient, NoWarnWhileSendsOccurEveryOneSecond) {
    SendTracker tracker;
    tracker.EnterInGame(0);
    for (std::uint64_t t = 1; t <= kSendsInRun; ++t) {
        tracker.RecordSend(t * kSecond);
        EXPECT_FALSE(tracker.CheckNoSend(t * kSecond + kSecond / 2).has_value()) << "t=" << t;
    }
    EXPECT_EQ(tracker.Sends(), kSendsInRun);
}

}  // namespace

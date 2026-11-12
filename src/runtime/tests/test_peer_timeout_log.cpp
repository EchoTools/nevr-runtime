// Peer timeout logging: the server's communication-timeout line and the client's in-game send state and no-send
// warning (runtime/compat/peer_timeout_log.h). Pure: every time is passed in; no sleep, no clock.
//
// Mutants (one line each, run against peer_timeout_log.cpp):
//   M1 FormatCommunicationTimeout: gap = now - last (clamp dropped)   -> ClampsAClockThatReadsEarlier fails.
//   M2 FormatCommunicationTimeout: print gap_s from the wrong field   -> ServerLine names the gap 15.033.
//   M3 RecordSend: sends_ not incremented                              -> SendStateCountsSends fails.
//   M4 CheckNoSend: `>=` changed to `>`                                -> WarnsExactlyAtFiveSeconds fails.
//   M5 CheckNoSend: warned_ never set                                  -> WarnsOncePerSilence fails.
//   M6 RecordSend: warned_ not cleared                                 -> RearmsAfterASend fails.
//   M7 CheckNoSend: returns a value when not in game                   -> NotInGameNeverWarns fails.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "runtime/compat/peer_timeout_log.h"

namespace {

using nevr_peer_timeout_log::CommTimeoutRecord;
using nevr_peer_timeout_log::FormatCommunicationTimeout;
using nevr_peer_timeout_log::FormatInGameSendState;
using nevr_peer_timeout_log::FormatNoSendWarning;
using nevr_peer_timeout_log::SendTracker;
using nevr_peer_timeout_log::kNoSendWarnMicros;

constexpr std::uint64_t kSecond = 1'000'000ULL;

// The numbers of the observed run (docs in the trace): last server record for peer 0 at 09:36:37.029, removal at
// 09:36:52.062, a gap of 15.033 s. Stamps are relative microseconds here.
constexpr std::uint64_t kLastReceive = 37'029'000ULL;
constexpr std::uint64_t kRemoval = 52'062'000ULL;

TEST(PeerTimeoutServer, ServerLineNamesPeerSlotLastReceiveAndGap) {
    const CommTimeoutRecord record{0, "10.0.8.50:6794", kLastReceive, kRemoval};
    const std::string line = FormatCommunicationTimeout(record);
    EXPECT_EQ(line.rfind("[NEVR.GAMESERVER] ", 0), 0u) << line;
    EXPECT_NE(line.find("peer communication timeout"), std::string::npos) << line;
    EXPECT_NE(line.find("slot=0"), std::string::npos) << line;
    EXPECT_NE(line.find("address=10.0.8.50:6794"), std::string::npos) << line;
    EXPECT_NE(line.find("last_receive_us=37029000"), std::string::npos) << line;
    EXPECT_NE(line.find("gap_s=15.033"), std::string::npos) << line;
    EXPECT_EQ(line.find('\n'), std::string::npos) << "a record is one line";
}

TEST(PeerTimeoutServer, ClampsAClockThatReadsEarlier) {
    const CommTimeoutRecord record{3, "10.0.0.1:1", 900, 100};
    const std::string line = FormatCommunicationTimeout(record);
    EXPECT_NE(line.find("gap_s=0.000"), std::string::npos) << line;
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
    for (std::uint64_t t = 1; t <= 30; ++t) {
        tracker.RecordSend(t * kSecond);
        EXPECT_FALSE(tracker.CheckNoSend(t * kSecond + kSecond / 2).has_value()) << "t=" << t;
    }
    EXPECT_EQ(tracker.Sends(), 30u);
}

}  // namespace

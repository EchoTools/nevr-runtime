// #1401 (D3 of #906), docs/adr/0017 ("Join"): the decisions of the join of a resolved invite code, and the staging of the lobby
// id in the game object, on the host with a fake seam and a fake memory. Header only (patch/invite_join.h): no game, no thread.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "runtime/patch/invite_join.h"

namespace {

using nevr_invite::LogLevel;
using nevr_invite_join::Joiner;
using nevr_invite_join::LobbyBytes;
using nevr_invite_join::Reason;
using nevr_invite_join::Seam;

constexpr const char* kMatch = "12345678-9abc-def0-1122-334455667788";
constexpr const char* kMatchUpper = "12345678-9ABC-DEF0-1122-334455667788";

// The Windows GUID bytes of kMatch: Data1 0x12345678, Data2 0x9abc, Data3 0xdef0 (little-endian), then Data4 in order.
const LobbyBytes kMatchBytes = {0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a, 0xf0, 0xde, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

// A fake of the game side: each callback records what it was asked and returns what the test sets.
struct Fake {
  bool game_thread = true;
  bool prologues = true;
  std::optional<EchoVR::NetGameState> state = EchoVR::NetGameState::LoggedIn;
  bool stage_ok = true;
  std::int64_t now = 1000;
  std::vector<std::string> events;  // "stage", "begin", in the order they happened
  std::vector<LobbyBytes> staged_lobby;
  std::vector<std::uint16_t> staged_team;
  std::vector<std::pair<LogLevel, std::string>> logs;

  Seam MakeSeam() {
    Seam seam;
    seam.on_game_thread = [this] { return game_thread; };
    seam.prologues_valid = [this] { return prologues; };
    seam.net_state = [this] { return state; };
    seam.stage_lobby = [this](const LobbyBytes& lobby, std::uint16_t team) {
      events.push_back("stage");
      staged_lobby.push_back(lobby);
      staged_team.push_back(team);
      return stage_ok;
    };
    seam.begin = [this] { events.push_back("begin"); };
    seam.now_ms = [this] { return now; };
    seam.log = [this](LogLevel level, const std::string& line) { logs.emplace_back(level, line); };
    return seam;
  }

  int Begins() const {
    int n = 0;
    for (const std::string& e : events) n += e == "begin" ? 1 : 0;
    return n;
  }

  bool Logged(LogLevel level, const std::string& line) const {
    for (const auto& [l, text] : logs) {
      if (l == level && text == line) return true;
    }
    return false;
  }
};

// The game object's memory for the staging tests: bytes at their offsets, with switches for a write that is accepted and not
// kept, a write or a read that is refused.
class FakeMemory final : public nevr_invite_join::StagingMemory {
 public:
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x9000, 0);
  bool drop_lobby = false;   // accepted, not stored
  bool drop_team = false;    // accepted, not stored
  bool refuse_write = false;
  bool refuse_read = false;

  bool Read(std::uintptr_t offset, void* out, std::size_t size) override {
    if (refuse_read || offset + size > bytes.size()) return false;
    std::memcpy(out, bytes.data() + offset, size);
    return true;
  }
  bool Write(std::uintptr_t offset, const void* in, std::size_t size) override {
    if (refuse_write || offset + size > bytes.size()) return false;
    if ((offset == nevr_invite_join::kStagedLobbyOffset && drop_lobby) ||
        (offset == nevr_invite_join::kStagedTeamOffset && drop_team)) {
      return true;
    }
    std::memcpy(bytes.data() + offset, in, size);
    return true;
  }
};

LobbyBytes Lobby(FakeMemory& m) {
  LobbyBytes out{};
  std::memcpy(out.data(), m.bytes.data() + nevr_invite_join::kStagedLobbyOffset, out.size());
  return out;
}

std::uint16_t Team(FakeMemory& m) {
  std::uint16_t out = 0;
  std::memcpy(&out, m.bytes.data() + nevr_invite_join::kStagedTeamOffset, sizeof(out));
  return out;
}

// What the game holds before any join: a non-zero block (its own invalid id), so a restore that writes zeros would show.
void FillPrior(FakeMemory& m) {
  for (std::size_t i = 0; i < 16; ++i) m.bytes[nevr_invite_join::kStagedLobbyOffset + i] = static_cast<std::uint8_t>(0xA0 + i);
  m.bytes[nevr_invite_join::kStagedTeamOffset] = 0x34;
  m.bytes[nevr_invite_join::kStagedTeamOffset + 1] = 0x12;
  // Bytes after the team word: a 32-bit team write would touch them, a 16-bit one must not.
  m.bytes[nevr_invite_join::kStagedTeamOffset + 2] = 0xAA;
  m.bytes[nevr_invite_join::kStagedTeamOffset + 3] = 0xBB;
}

std::vector<std::uint8_t> Snapshot(const FakeMemory& m) { return m.bytes; }

}  // namespace

// ---- The lobby id: the Windows GUID layout -------------------------------------------------------------------------------------

TEST(InviteJoinLobbyId, TheWindowsGuidLayoutIsDataOneTwoThreeLittleEndianThenDataFourInOrder) {
  LobbyBytes out{};
  ASSERT_TRUE(nevr_invite_join::ParseLobbyBytes(kMatch, out));
  EXPECT_EQ(out, kMatchBytes);
}

TEST(InviteJoinLobbyId, UpperCaseHexParsesToTheSameBytes) {
  LobbyBytes lower{};
  LobbyBytes upper{};
  ASSERT_TRUE(nevr_invite_join::ParseLobbyBytes("abcdef01-2345-6789-abcd-ef0123456789", lower));
  ASSERT_TRUE(nevr_invite_join::ParseLobbyBytes("ABCDEF01-2345-6789-ABCD-EF0123456789", upper));
  EXPECT_EQ(lower, upper);
  // Data1 0xabcdef01 is stored little-endian: 01 ef cd ab.
  EXPECT_EQ(lower[0], 0x01);
  EXPECT_EQ(lower[1], 0xef);
  EXPECT_EQ(lower[2], 0xcd);
  EXPECT_EQ(lower[3], 0xab);
}

TEST(InviteJoinLobbyId, WhatIsNotAUuidIsRefused) {
  LobbyBytes out{};
  EXPECT_FALSE(nevr_invite_join::ParseLobbyBytes("", out));
  EXPECT_FALSE(nevr_invite_join::ParseLobbyBytes("12345678-9abc-def0-1122-33445566778", out));    // 35 characters
  EXPECT_FALSE(nevr_invite_join::ParseLobbyBytes("12345678-9abc-def0-1122-3344556677889", out));  // 37 characters
  EXPECT_FALSE(nevr_invite_join::ParseLobbyBytes("12345678_9abc-def0-1122-334455667788", out));   // hyphen missing
  EXPECT_FALSE(nevr_invite_join::ParseLobbyBytes("1234567g-9abc-def0-1122-334455667788", out));   // not hex
}

TEST(InviteJoinLobbyId, TheLogFormIsUpperCase) {
  EXPECT_EQ(nevr_invite_join::UpperUuid(kMatch), kMatchUpper);
}

// ---- The pinned layout ----------------------------------------------------------------------------------------------------------

TEST(InviteJoinLayout, TheOffsetsAndTheTeamWordAreTheGamesOwn) {
  EXPECT_EQ(nevr_invite_join::kStagedLobbyOffset, 0x8540u);  // CR15Game::BeginMultiplayer, lea 0x8540(%rbx)
  EXPECT_EQ(nevr_invite_join::kStagedTeamOffset, 0x8560u);   // movzwl 0x8560(%rbx)
  EXPECT_EQ(nevr_invite_join::kTeamWord, 0xFFFFu);
  EXPECT_EQ(sizeof(LobbyBytes), 16u);
}

TEST(InviteJoinLayout, TheReasonsHaveTheIssuesNames) {
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::Off), "off");
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::WrongThread), "wrong_thread");
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::Prologue), "prologue");
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::Busy), "busy");
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::NotAtMenu), "not_at_menu");
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::StageFailed), "stage_failed");
}

TEST(InviteJoinLayout, APrologueOrAStageThatFailsIsAWarningAndTheExpectedRefusalsAreInfo) {
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::Prologue), LogLevel::Warning);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::StageFailed), LogLevel::Warning);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::Off), LogLevel::Info);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::WrongThread), LogLevel::Info);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::Busy), LogLevel::Info);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::NotAtMenu), LogLevel::Info);
}

// ---- The staging: write, read back, undo on any failure -------------------------------------------------------------------------

TEST(InviteJoinStage, TheLobbyAndTheTeamAreWrittenAndReadBackAsWritten) {
  FakeMemory m;
  FillPrior(m);
  ASSERT_TRUE(nevr_invite_join::StageAndVerify(m, kMatchBytes, nevr_invite_join::kTeamWord));
  EXPECT_EQ(Lobby(m), kMatchBytes);
  EXPECT_EQ(Team(m), 0xFFFFu);
}

TEST(InviteJoinStage, ATeamWordIsSixteenBitsAndTouchesNoByteAfterIt) {
  FakeMemory m;
  FillPrior(m);
  ASSERT_TRUE(nevr_invite_join::StageAndVerify(m, kMatchBytes, nevr_invite_join::kTeamWord));
  EXPECT_EQ(m.bytes[nevr_invite_join::kStagedTeamOffset + 2], 0xAA);
  EXPECT_EQ(m.bytes[nevr_invite_join::kStagedTeamOffset + 3], 0xBB);
}

TEST(InviteJoinStage, ALobbyThatDoesNotLandIsUndoneAndSaysSo) {
  FakeMemory m;
  FillPrior(m);
  const std::vector<std::uint8_t> before = Snapshot(m);
  m.drop_lobby = true;
  EXPECT_FALSE(nevr_invite_join::StageAndVerify(m, kMatchBytes, nevr_invite_join::kTeamWord));
  EXPECT_EQ(Snapshot(m), before);  // the prior bytes are back, team included
}

TEST(InviteJoinStage, ATeamThatDoesNotLandIsUndoneAndTheLobbyIsRestoredToo) {
  FakeMemory m;
  FillPrior(m);
  const std::vector<std::uint8_t> before = Snapshot(m);
  m.drop_team = true;
  EXPECT_FALSE(nevr_invite_join::StageAndVerify(m, kMatchBytes, nevr_invite_join::kTeamWord));
  EXPECT_EQ(Snapshot(m), before);
}

TEST(InviteJoinStage, AWriteTheGameObjectRefusesLeavesItAsItWas) {
  FakeMemory m;
  FillPrior(m);
  const std::vector<std::uint8_t> before = Snapshot(m);
  m.refuse_write = true;
  EXPECT_FALSE(nevr_invite_join::StageAndVerify(m, kMatchBytes, nevr_invite_join::kTeamWord));
  EXPECT_EQ(Snapshot(m), before);
}

TEST(InviteJoinStage, AReadThatFailsStagesNothing) {
  FakeMemory m;
  FillPrior(m);
  const std::vector<std::uint8_t> before = Snapshot(m);
  m.refuse_read = true;
  EXPECT_FALSE(nevr_invite_join::StageAndVerify(m, kMatchBytes, nevr_invite_join::kTeamWord));
  EXPECT_EQ(Snapshot(m), before);
}

// ---- The decisions: one join per answer, from the menu, on the game's thread, with the prologues checked ----------------------

TEST(InviteJoinOffer, AResolvedMatchStagesItsLobbyThenBeginsOnceAfterTheStage) {
  Fake f;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::None);
  EXPECT_EQ(f.events, (std::vector<std::string>{"stage", "begin"}));
  ASSERT_EQ(f.staged_lobby.size(), 1u);
  EXPECT_EQ(f.staged_lobby[0], kMatchBytes);
  EXPECT_EQ(f.staged_team[0], 0xFFFFu);
  EXPECT_TRUE(f.Logged(LogLevel::Info, std::string("[NEVR.INVITE] join started route=stage lobby=") + kMatchUpper + " state_from=3"));
}

TEST(InviteJoinOffer, ARepeatForTheSameLobbyInsideTheWindowCallsNothingAndSaysBusy) {
  Fake f;
  Joiner joiner(f.MakeSeam());
  ASSERT_EQ(joiner.Offer(kMatch, true), Reason::None);
  f.now += 1000;
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::Busy);
  EXPECT_EQ(f.Begins(), 1);
  EXPECT_EQ(f.events.size(), 2u);  // no second stage either
  EXPECT_TRUE(f.Logged(LogLevel::Info, std::string("[NEVR.INVITE] join not started reason=busy lobby=") + kMatchUpper));
}

TEST(InviteJoinOffer, AnotherLobbyInsideTheWindowAlsoWaitsBecauseTwoBeginsAtOnceAreOneHazard) {
  Fake f;
  Joiner joiner(f.MakeSeam());
  ASSERT_EQ(joiner.Offer(kMatch, true), Reason::None);
  f.now += 1000;
  EXPECT_EQ(joiner.Offer("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", true), Reason::Busy);
  EXPECT_EQ(f.Begins(), 1);
}

TEST(InviteJoinOffer, TheWindowEndsAtItsBoundAndAJoinIsAllowedAgain) {
  Fake f;
  Joiner joiner(f.MakeSeam());
  ASSERT_EQ(joiner.Offer(kMatch, true), Reason::None);
  f.now = 1000 + nevr_invite_join::kJoinWindowMs - 1;
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::Busy);
  f.now = 1000 + nevr_invite_join::kJoinWindowMs;
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::None);
  EXPECT_EQ(f.Begins(), 2);
}

TEST(InviteJoinOffer, AStateOtherThanLoggedInCallsNothingAndSaysWhichStateItWas) {
  const std::vector<std::optional<EchoVR::NetGameState>> states = {
      EchoVR::NetGameState::LoggedOut, EchoVR::NetGameState::LoadingRoot, EchoVR::NetGameState::LoggingIn,
      std::nullopt};
  for (const auto& state : states) {
    Fake f;
    f.state = state;
    Joiner joiner(f.MakeSeam());
    EXPECT_EQ(joiner.Offer(kMatch, true), Reason::NotAtMenu);
    EXPECT_TRUE(f.events.empty()) << "a state that is not LoggedIn stages and begins nothing";
    EXPECT_EQ(f.Begins(), 0);
    bool said = false;
    for (const auto& [level, line] : f.logs) {
      if (line.find("reason=not_at_menu") != std::string::npos) said = true;
    }
    EXPECT_TRUE(said);
  }
}

TEST(InviteJoinOffer, AStateOtherThanLoggedInNamesTheStateItWasAndNoStateWhenThereIsNoNetGame) {
  Fake f;
  f.state = EchoVR::NetGameState::LoggingIn;
  Joiner joiner(f.MakeSeam());
  joiner.Offer(kMatch, true);
  EXPECT_TRUE(f.Logged(LogLevel::Info, std::string("[NEVR.INVITE] join not started reason=not_at_menu lobby=") + kMatchUpper +
                                           " state_from=2"));

  Fake none;
  none.state = std::nullopt;
  Joiner joiner2(none.MakeSeam());
  joiner2.Offer(kMatch, true);
  EXPECT_TRUE(none.Logged(LogLevel::Info, std::string("[NEVR.INVITE] join not started reason=not_at_menu lobby=") + kMatchUpper +
                                              " state_from=none"));
}

TEST(InviteJoinOffer, ANonGameThreadCallsNothingAndSaysSo) {
  Fake f;
  f.game_thread = false;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::WrongThread);
  EXPECT_TRUE(f.events.empty());
  EXPECT_TRUE(f.Logged(LogLevel::Info, std::string("[NEVR.INVITE] join not started reason=wrong_thread lobby=") + kMatchUpper));
}

TEST(InviteJoinOffer, AFeatureThatIsOffCallsNothingAndSaysSo) {
  Fake f;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, false), Reason::Off);
  EXPECT_TRUE(f.events.empty());
  EXPECT_TRUE(f.Logged(LogLevel::Info, std::string("[NEVR.INVITE] join not started reason=off lobby=") + kMatchUpper));
}

TEST(InviteJoinOffer, APrologueMismatchCallsNothingAndSaysSoAsAWarning) {
  Fake f;
  f.prologues = false;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::Prologue);
  EXPECT_TRUE(f.events.empty());
  EXPECT_TRUE(f.Logged(LogLevel::Warning, std::string("[NEVR.INVITE] join not started reason=prologue lobby=") + kMatchUpper));
}

TEST(InviteJoinOffer, AStagingRefusalCallsNothingSaysSoAndOpensNoWindow) {
  Fake f;
  f.stage_ok = false;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::StageFailed);
  EXPECT_EQ(f.Begins(), 0);
  EXPECT_TRUE(f.Logged(LogLevel::Warning, std::string("[NEVR.INVITE] join not started reason=stage_failed lobby=") + kMatchUpper));
  // The refused stage opened no window: the same lobby, staged now, begins.
  f.stage_ok = true;
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::None);
  EXPECT_EQ(f.Begins(), 1);
}

TEST(InviteJoinOffer, AnUnparsableIdIsAStagingRefusalAndItsTextIsNotLogged) {
  Fake f;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer("not-a-match-id-from-the-service", true), Reason::StageFailed);
  EXPECT_TRUE(f.events.empty());
  EXPECT_TRUE(f.Logged(LogLevel::Warning, "[NEVR.INVITE] join not started reason=stage_failed lobby=invalid"));
  for (const auto& [level, line] : f.logs) {
    EXPECT_EQ(line.find("not-a-match-id"), std::string::npos) << "remote text is never logged: " << line;
  }
}

TEST(InviteJoinOffer, TheGatesAreCheckedInTheirOrderSoOneReasonIsLogged) {
  Fake f;
  f.game_thread = false;
  f.prologues = false;
  f.state = std::nullopt;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, false), Reason::Off);  // off first, whatever else is wrong
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::WrongThread);
}

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
  bool server = false;
  bool game_thread = true;
  bool prologues = true;
  std::optional<EchoVR::NetGameState> state = EchoVR::NetGameState::LoggedIn;
  bool stage_ok = true;
  nevr_invite_join::StageOutcome stage_outcome = [] {  // returned when stage_ok is false
    nevr_invite_join::StageOutcome o;
    o.step = nevr_invite_join::StageStep::Write;
    o.field = nevr_invite_join::StageField::Lobby;
    return o;
  }();
  bool begin_ok = true;
  bool unstage_ok = true;
  int unstages = 0;
  std::int64_t now = 1000;
  std::vector<std::string> events;  // "stage", "begin", in the order they happened
  std::vector<LobbyBytes> staged_lobby;
  std::vector<std::uint16_t> staged_team;
  std::vector<std::pair<LogLevel, std::string>> logs;

  Seam MakeSeam() {
    Seam seam;
    seam.is_server = [this] { return server; };
    seam.on_game_thread = [this] { return game_thread; };
    seam.prologues_valid = [this] { return prologues; };
    seam.net_state = [this] { return state; };
    seam.stage_lobby = [this](const LobbyBytes& lobby, std::uint16_t team) {
      events.push_back("stage");
      staged_lobby.push_back(lobby);
      staged_team.push_back(team);
      nevr_invite_join::StageOutcome outcome;
      if (!stage_ok) outcome = stage_outcome;
      return outcome;
    };
    seam.begin = [this] {
      events.push_back("begin");
      return begin_ok;
    };
    seam.unstage = [this] {
      ++unstages;
      return unstage_ok;
    };
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
  int writes_before_refusal = -1;  // when >= 0, writes after this many are refused
  int write_count = 0;

  bool Read(std::uintptr_t offset, void* out, std::size_t size) override {
    if (refuse_read || offset + size > bytes.size()) return false;
    std::memcpy(out, bytes.data() + offset, size);
    return true;
  }
  bool Write(std::uintptr_t offset, const void* in, std::size_t size) override {
    if (refuse_write || offset + size > bytes.size()) return false;
    if (writes_before_refusal >= 0 && write_count++ >= writes_before_refusal) return false;
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

// What a stage in the tests logs: the lines, by level.
struct Logs {
  std::vector<std::pair<LogLevel, std::string>> lines;
  nevr_invite_join::LogFn Fn() {
    return [this](LogLevel level, const std::string& line) { lines.emplace_back(level, line); };
  }
  bool Has(LogLevel level, const std::string& line) const {
    for (const auto& [l, text] : lines) {
      if (l == level && text == line) return true;
    }
    return false;
  }
};

nevr_invite_join::StageOutcome Stage(FakeMemory& m, Logs& logs) {
  return nevr_invite_join::StageAndVerify(m, kMatchBytes, nevr_invite_join::kTeamWord, logs.Fn());
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
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::Server), "server");
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::OtherRoute), "other_route");
  EXPECT_STREQ(nevr_invite_join::ReasonName(Reason::NoGameObject), "no_game_object");
}

TEST(InviteJoinLayout, APrologueOrAStageThatFailsIsAWarningAndTheExpectedRefusalsAreInfo) {
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::Prologue), LogLevel::Warning);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::StageFailed), LogLevel::Warning);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::NoGameObject), LogLevel::Warning);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::OtherRoute), LogLevel::Info);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::Server), LogLevel::Info);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::Off), LogLevel::Info);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::WrongThread), LogLevel::Info);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::Busy), LogLevel::Info);
  EXPECT_EQ(nevr_invite_join::LevelOf(Reason::NotAtMenu), LogLevel::Info);
}

// ---- The staging: write, read back, undo on any failure -------------------------------------------------------------------------

TEST(InviteJoinStage, TheLobbyAndTheTeamAreWrittenAndReadBackAsWritten) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  ASSERT_TRUE(Stage(m, logs).ok());
  EXPECT_EQ(Lobby(m), kMatchBytes);
  EXPECT_EQ(Team(m), 0xFFFFu);
}

TEST(InviteJoinStage, ATeamWordIsSixteenBitsAndTouchesNoByteAfterIt) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  ASSERT_TRUE(Stage(m, logs).ok());
  EXPECT_EQ(m.bytes[nevr_invite_join::kStagedTeamOffset + 2], 0xAA);
  EXPECT_EQ(m.bytes[nevr_invite_join::kStagedTeamOffset + 3], 0xBB);
}

TEST(InviteJoinStage, ALobbyThatDoesNotLandIsUndoneAndSaysSo) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  const std::vector<std::uint8_t> before = Snapshot(m);
  m.drop_lobby = true;
  EXPECT_FALSE(Stage(m, logs).ok());
  EXPECT_EQ(Snapshot(m), before);  // the prior bytes are back, team included
}

TEST(InviteJoinStage, ATeamThatDoesNotLandIsUndoneAndTheLobbyIsRestoredToo) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  const std::vector<std::uint8_t> before = Snapshot(m);
  m.drop_team = true;
  EXPECT_FALSE(Stage(m, logs).ok());
  EXPECT_EQ(Snapshot(m), before);
}

TEST(InviteJoinStage, AWriteTheGameObjectRefusesLeavesItAsItWas) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  const std::vector<std::uint8_t> before = Snapshot(m);
  m.refuse_write = true;
  EXPECT_FALSE(Stage(m, logs).ok());
  EXPECT_EQ(Snapshot(m), before);
}

TEST(InviteJoinStage, AReadThatFailsStagesNothing) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  const std::vector<std::uint8_t> before = Snapshot(m);
  m.refuse_read = true;
  EXPECT_FALSE(Stage(m, logs).ok());
  EXPECT_EQ(Snapshot(m), before);
}

// ---- The decisions: one join per answer, from the menu, on the game's thread, with the prologues checked ----------------------

// ---- The other routes of the game's own join ------------------------------------------------------------------------------------

namespace {
void SetRoute(FakeMemory& m, std::uint8_t flags, std::uint64_t slot, const LobbyBytes& lobby) {
  m.bytes[nevr_invite_join::kRouteFlagsOffset] = flags;
  std::memcpy(m.bytes.data() + nevr_invite_join::kRouteSlotOffset, &slot, sizeof(slot));
  std::memcpy(m.bytes.data() + nevr_invite_join::kRouteLobbyOffset, lobby.data(), lobby.size());
}
}  // namespace

TEST(InviteJoinRoute, TheInvalidIdIsSixteenZeroBytesAndTheRouteOffsetsAreTheGamesOwn) {
  EXPECT_EQ(nevr_invite_join::kInvalidLobby, LobbyBytes{});
  EXPECT_EQ(nevr_invite_join::kRouteFlagsOffset, 0x8520u);
  EXPECT_EQ(nevr_invite_join::kRouteSlotOffset, 0x8528u);
  EXPECT_EQ(nevr_invite_join::kRouteLobbyOffset, 0x8530u);
}

TEST(InviteJoinRoute, AnIdleRouteStages) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  SetRoute(m, 0x00, 0, LobbyBytes{});
  EXPECT_TRUE(Stage(m, logs).ok());
}

TEST(InviteJoinRoute, AFlagNibbleWithoutASlotOrASlotWithoutAFlagNibbleStillStages) {
  for (const auto& [flags, slot] : std::vector<std::pair<std::uint8_t, std::uint64_t>>{{0x01, 0}, {0xF0, 5}, {0x00, 5}}) {
    FakeMemory m;
    Logs logs;
    FillPrior(m);
    SetRoute(m, flags, slot, LobbyBytes{});
    EXPECT_TRUE(Stage(m, logs).ok()) << "flags=" << static_cast<int>(flags) << " slot=" << slot;
  }
}

TEST(InviteJoinRoute, AFlagNibbleAndASlotSendTheJoinElsewhereAndNothingIsStaged) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  SetRoute(m, 0x02, 7, LobbyBytes{});
  const std::vector<std::uint8_t> before = Snapshot(m);
  const nevr_invite_join::StageOutcome outcome = Stage(m, logs);
  EXPECT_EQ(outcome.step, nevr_invite_join::StageStep::OtherRoute);
  EXPECT_EQ(outcome.route.flags, 0x02);
  EXPECT_EQ(outcome.route.slot, 7u);
  EXPECT_EQ(Snapshot(m), before);
}

TEST(InviteJoinRoute, AnotherLobbyAtTheRouteFieldSendsTheJoinElsewhereAndNothingIsStaged) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  SetRoute(m, 0x00, 0, kMatchBytes);
  const std::vector<std::uint8_t> before = Snapshot(m);
  const nevr_invite_join::StageOutcome outcome = Stage(m, logs);
  EXPECT_EQ(outcome.step, nevr_invite_join::StageStep::OtherRoute);
  EXPECT_EQ(outcome.route.lobby, kMatchBytes);
  EXPECT_EQ(Snapshot(m), before);
}

TEST(InviteJoinRoute, ALobbyTextRoundTripsTheGuidBytes) {
  EXPECT_EQ(nevr_invite_join::LobbyText(kMatchBytes), kMatchUpper);
}

TEST(InviteJoinRoute, TheOffersLineNamesTheValuesRead) {
  Fake f;
  f.stage_ok = false;
  f.stage_outcome.step = nevr_invite_join::StageStep::OtherRoute;
  f.stage_outcome.field = nevr_invite_join::StageField::RouteLobby;
  f.stage_outcome.route.flags = 0x12;
  f.stage_outcome.route.slot = 0xAB;
  f.stage_outcome.route.lobby = kMatchBytes;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::OtherRoute);
  EXPECT_EQ(f.Begins(), 0);
  EXPECT_TRUE(f.Logged(LogLevel::Info, std::string("[NEVR.INVITE] join not started reason=other_route lobby=") + kMatchUpper +
                                           " route_flags=0x12 route_slot=0xab route_lobby=" + kMatchUpper));
}

// ---- A failed undo, the failing step, the server guard, a vanished game object --------------------------------------------------

TEST(InviteJoinStage, AFailedUndoIsLoggedAtErrorAndNamesTheField) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  m.drop_team = true;            // the team does not land, so the stage undoes
  m.writes_before_refusal = 2;   // the two stage writes land, both undo writes are refused
  EXPECT_FALSE(Stage(m, logs).ok());
  EXPECT_TRUE(logs.Has(LogLevel::Error, "[NEVR.INVITE] join stage undo failed field=lobby step=write"));
  EXPECT_TRUE(logs.Has(LogLevel::Error, "[NEVR.INVITE] join stage undo failed field=team step=write"));
}

TEST(InviteJoinStage, ASuccessfulUndoLogsNothing) {
  FakeMemory m;
  Logs logs;
  FillPrior(m);
  m.drop_lobby = true;
  EXPECT_FALSE(Stage(m, logs).ok());
  EXPECT_TRUE(logs.lines.empty());
}

TEST(InviteJoinStage, TheFailingStepAndFieldAreNamed) {
  {
    FakeMemory m;
    Logs logs;
    m.drop_lobby = true;
    const auto o = Stage(m, logs);
    EXPECT_EQ(o.step, nevr_invite_join::StageStep::Mismatch);
    EXPECT_EQ(o.field, nevr_invite_join::StageField::Lobby);
  }
  {
    FakeMemory m;
    Logs logs;
    m.drop_team = true;
    const auto o = Stage(m, logs);
    EXPECT_EQ(o.step, nevr_invite_join::StageStep::Mismatch);
    EXPECT_EQ(o.field, nevr_invite_join::StageField::Team);
  }
  {
    FakeMemory m;
    Logs logs;
    m.refuse_write = true;
    const auto o = Stage(m, logs);
    EXPECT_EQ(o.step, nevr_invite_join::StageStep::Write);
    EXPECT_EQ(o.field, nevr_invite_join::StageField::Lobby);
  }
  {
    FakeMemory m;
    Logs logs;
    m.refuse_read = true;
    const auto o = Stage(m, logs);
    EXPECT_EQ(o.step, nevr_invite_join::StageStep::Read);
    EXPECT_EQ(o.field, nevr_invite_join::StageField::RouteFlags);
  }
}

TEST(InviteJoinOffer, AStageFailureLineNamesTheStepAndTheField) {
  Fake f;
  f.stage_ok = false;
  f.stage_outcome.step = nevr_invite_join::StageStep::Write;
  f.stage_outcome.field = nevr_invite_join::StageField::Team;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::StageFailed);
  EXPECT_TRUE(f.Logged(LogLevel::Warning, std::string("[NEVR.INVITE] join not started reason=stage_failed lobby=") + kMatchUpper +
                                              " step=write field=team"));
}

TEST(InviteJoinOffer, ADedicatedServerNeverJoinsByCode) {
  Fake f;
  f.server = true;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::Server);
  EXPECT_TRUE(f.events.empty());
  EXPECT_TRUE(f.Logged(LogLevel::Info, std::string("[NEVR.INVITE] join not started reason=server lobby=") + kMatchUpper));
}

TEST(InviteJoinOffer, AGameObjectThatVanishesAfterTheStageIsUndoneAndTheWindowStaysClosed) {
  Fake f;
  f.begin_ok = false;
  Joiner joiner(f.MakeSeam());
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::NoGameObject);
  EXPECT_EQ(f.unstages, 1);
  EXPECT_TRUE(f.Logged(LogLevel::Warning, std::string("[NEVR.INVITE] join not started reason=no_game_object lobby=") + kMatchUpper));
  f.begin_ok = true;
  EXPECT_EQ(joiner.Offer(kMatch, true), Reason::None);  // no window was left open
}

TEST(InviteJoinOffer, AnUndoThatFailsAfterAVanishedGameObjectIsLoggedAtError) {
  Fake f;
  f.begin_ok = false;
  f.unstage_ok = false;
  Joiner joiner(f.MakeSeam());
  joiner.Offer(kMatch, true);
  EXPECT_TRUE(f.Logged(LogLevel::Error, "[NEVR.INVITE] join stage undo failed after begin step=unstage field=lobby"));
}

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
  EXPECT_TRUE(f.Logged(LogLevel::Warning, std::string("[NEVR.INVITE] join not started reason=stage_failed lobby=") + kMatchUpper +
                                              " step=write field=lobby"));
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
  EXPECT_TRUE(f.Logged(LogLevel::Warning, "[NEVR.INVITE] join not started reason=stage_failed lobby=invalid step=parse field=lobby"));
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

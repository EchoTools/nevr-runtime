// Host test for the lobby-full policy (quest/social/lobby_full_backoff.h, #1001). The same cases as the PC policy's
// (#889, runtime/tests/test_lobby_full_backoff.cpp), on the Quest's 16-byte lobby key. The policy is pure: no clock,
// no lock, no game. The hooks that drive it on libr15 are checked on the device, not here.
//
// Run: lobby_full_test [name-filter]

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mini_test.h"
#include "quest/social/lobby_full_backoff.h"
#include "quest/social/lobby_full_layout.h"
#include "runtime/patch/lobby_failure_message_rules.h"

namespace {

using quest_social::lobby_full::FullLobbyBackoff;
using quest_social::lobby_full::LobbyKey;

LobbyKey Key(std::uint8_t seed) {
  LobbyKey key{};
  for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(seed + i);
  return key;
}

const LobbyKey kLobby = Key(0x11);
const LobbyKey kOther = Key(0x77);

TEST(AJoinNeverRefusedIsSent) {
  FullLobbyBackoff b;
  const auto d = b.OnJoin(kLobby, 0);
  CHECK(d.verdict == FullLobbyBackoff::Verdict::kSend);
  CHECK_EQ(d.refusals, 0U);
}

TEST(TheFirstRefusalWaitsOneSecond) {
  FullLobbyBackoff b;
  const auto r = b.OnFull(kLobby, 0);
  CHECK(r.verdict == FullLobbyBackoff::Verdict::kWait);
  CHECK_EQ(r.waitMs, 1000U);
  CHECK(b.OnJoin(kLobby, 999).verdict == FullLobbyBackoff::Verdict::kWait);
  CHECK_EQ(b.OnJoin(kLobby, 999).waitMs, 1U);
  CHECK(b.OnJoin(kLobby, 1000).verdict == FullLobbyBackoff::Verdict::kSend);
}

TEST(WaitsDoubleFromOneSecondAndStopAtThirtySeconds) {
  FullLobbyBackoff b;
  const std::vector<std::uint64_t> expected = {1000, 2000, 4000, 8000, 16000, 30000};
  std::uint64_t now = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto r = b.OnFull(kLobby, now);
    CHECK(r.verdict == FullLobbyBackoff::Verdict::kWait);
    CHECK_EQ(r.waitMs, expected[i]);
    now += expected[i];
  }
}

// The seventh refusal starts a cooldown of kCooldownMs (60 s, the PC policy's), not a permanent skip: no send until it
// ends, then the count starts again from the first wait.
TEST(TheSeventhRefusalStartsACooldownAndTheCountStartsAgain) {
  FullLobbyBackoff b;
  std::uint64_t now = 0;
  for (std::uint32_t i = 1; i < FullLobbyBackoff::kGiveUpAfter; ++i) now += b.OnFull(kLobby, now).waitMs;
  const auto last = b.OnFull(kLobby, now);
  CHECK(last.verdict == FullLobbyBackoff::Verdict::kGiveUp);
  CHECK_EQ(last.refusals, FullLobbyBackoff::kGiveUpAfter);
  CHECK_EQ(last.waitMs, 60000U);
  CHECK(b.OnJoin(kLobby, now + 1).verdict == FullLobbyBackoff::Verdict::kWait);
  CHECK_EQ(b.OnJoin(kLobby, now + 59999).waitMs, 1U);
  CHECK(b.OnJoin(kLobby, now + 60000).verdict == FullLobbyBackoff::Verdict::kSend);
  const auto next = b.OnFull(kLobby, now + 60000);
  CHECK(next.verdict == FullLobbyBackoff::Verdict::kWait);
  CHECK_EQ(next.refusals, 1U);
  CHECK_EQ(next.waitMs, 1000U);
}

// The same run as the PC policy's (lobby_full_backoff_rules.h): 7 sends, a 60 s cooldown, 7 more. Every send is a
// refusal here, the game ticks every 100 ms, and the window runs to 245 s.
TEST(ARefusedLobbyIsAskedInCappedRunsSeparatedByTheCooldown) {
  FullLobbyBackoff b;
  std::vector<std::uint64_t> sendTimes;
  for (std::uint64_t now = 0; now <= 245000; now += 100) {
    if (b.OnJoin(kLobby, now).verdict == FullLobbyBackoff::Verdict::kSend) {
      sendTimes.push_back(now);
      b.OnFull(kLobby, now);
    }
  }
  const std::vector<std::uint64_t> expected = {0,      1000,   3000,   7000,   15000,  31000,  61000,
                                               121000, 122000, 124000, 128000, 136000, 152000, 182000,
                                               242000, 243000, 245000};
  CHECK(sendTimes == expected);
}

// The code-8 page: the title is the code's own name and the service's message is line 2 (the shared rules, PC #1443).
TEST(CodeEightWritesTheLobbyFullPageWithTheServiceMessage) {
  namespace rules = nevr_lobby_failure_message;
  CHECK(rules::TakesServiceMessage(8));
  CHECK(std::string(rules::Title(8)) == "lobby full");
  static const std::size_t kSize = rules::kErrorBlockOffset + 1 + rules::kErrorLines * rules::kErrorLineBytes;
  std::vector<std::uint8_t> game(kSize, 0xAA);
  const char message[64] = "Lobby is full, try another";
  CHECK(rules::WriteErrorBlock(game.data(), rules::Title(8), message, sizeof message));
  std::uint8_t* block = game.data() + rules::kErrorBlockOffset;
  CHECK_EQ(block[0], rules::kErrorFlagFourLines);
  CHECK(std::string(reinterpret_cast<const char*>(block + 1)) == "lobby full");
  CHECK(std::string(reinterpret_cast<const char*>(block + 1 + rules::kErrorLineBytes)) == "Lobby is full, try another");
}

// The pointer arithmetic the failure hook does (lobby_full_layout.h), on a fake lobby client: +8 holds the CR15NetGame, +0x910
// holds a different object (LobbySessionFailureCB reads it as a table owner), and +0x448 is the status word. The error block
// must land in the object at +8 and nothing may be written to the one at +0x910.
TEST(TheErrorPageIsWrittenIntoTheGameAtPlusEightNotTheObjectAtPlus0x910) {
  namespace rules = nevr_lobby_failure_message;
  namespace lf = quest_social::lobby_full;
  const std::size_t kGameSize = rules::kErrorBlockOffset + 1 + rules::kErrorLines * rules::kErrorLineBytes;
  std::vector<std::uint8_t> game(kGameSize, 0xAA);
  std::vector<std::uint8_t> other(kGameSize, 0xBB);
  std::vector<std::uint8_t> client(0x1000, 0);
  std::uint8_t* gamePtr = game.data();
  std::uint8_t* otherPtr = other.data();
  std::memcpy(client.data() + 0x8, &gamePtr, sizeof gamePtr);
  std::memcpy(client.data() + 0x910, &otherPtr, sizeof otherPtr);
  const std::int32_t status = 5;  // inside the game's gate: status - 4 < 3
  std::memcpy(client.data() + 0x448, &status, sizeof status);

  // msg + 0x10 -> data; data + 0x18 is the code; data + 0x20 is the 64-byte message.
  std::vector<std::uint8_t> data(0x60, 0);
  data[0x18] = 8;
  std::memcpy(data.data() + 0x20, "Lobby is full, try another", sizeof "Lobby is full, try another");
  std::vector<std::uint8_t> msg(0x20, 0);
  const std::uint8_t* dataPtr = data.data();
  std::memcpy(msg.data() + 0x10, &dataPtr, sizeof dataPtr);

  std::int8_t code = 0;
  CHECK(lf::FailureCode(msg.data(), &code));
  CHECK_EQ(code, 8);
  const lf::RefusalReads reads = lf::ReadRefusal(client.data(), msg.data());
  CHECK(reads.gateOpen);
  CHECK(reads.game == game.data());
  CHECK(std::string(reads.message) == "Lobby is full, try another");

  CHECK(rules::WriteErrorBlock(reads.game, rules::Title(8), reads.message, sizeof reads.message));
  const std::uint8_t* block = game.data() + rules::kErrorBlockOffset;
  CHECK_EQ(block[0], rules::kErrorFlagFourLines);
  CHECK(std::string(reinterpret_cast<const char*>(block + 1)) == "lobby full");
  CHECK(std::string(reinterpret_cast<const char*>(block + 1 + rules::kErrorLineBytes)) == "Lobby is full, try another");
  CHECK(other == std::vector<std::uint8_t>(kGameSize, 0xBB));  // the object at +0x910 is untouched
}

TEST(TheStatusGateReadsTheWordAtPlus0x448) {
  namespace lf = quest_social::lobby_full;
  std::vector<std::uint8_t> client(0x1000, 0);
  for (std::int32_t status = 0; status < 10; ++status) {
    std::memcpy(client.data() + 0x448, &status, sizeof status);
    CHECK_EQ(lf::StatusGateOpen(client.data()), status >= 4 && status <= 6);
  }
  CHECK(!lf::StatusGateOpen(nullptr));
}

// The server refuses every send and the game ticks every 100 ms for 120 s. Without the backoff every tick sends
// (1200 sends). With it the sends are at 0, 1, 3, 7, 15, 31 and 61 s.
TEST(AFullLobbyIsAskedSevenTimesInSixtyOneSecondsNotEveryTick) {
  FullLobbyBackoff b;
  std::vector<std::uint64_t> sendTimes;
  for (std::uint64_t now = 0; now <= 120000; now += 100) {
    if (b.OnJoin(kLobby, now).verdict == FullLobbyBackoff::Verdict::kSend) {
      sendTimes.push_back(now);
      b.OnFull(kLobby, now);
    }
  }
  const std::vector<std::uint64_t> expected = {0, 1000, 3000, 7000, 15000, 31000, 61000};
  CHECK(sendTimes == expected);
}

TEST(ASuccessForgetsTheLobby) {
  FullLobbyBackoff b;
  b.OnFull(kLobby, 0);
  b.OnFull(kLobby, 1000);
  b.OnFull(kLobby, 3000);
  b.OnSuccess(kLobby);
  const auto d = b.OnJoin(kLobby, 3001);
  CHECK(d.verdict == FullLobbyBackoff::Verdict::kSend);
  CHECK_EQ(d.refusals, 0U);
}

TEST(AnotherLobbyIsNotHeldBack) {
  FullLobbyBackoff b;
  b.OnFull(kLobby, 0);
  CHECK(b.OnJoin(kOther, 1).verdict == FullLobbyBackoff::Verdict::kSend);
}

// The decline on the error page (#2006): the shared decision, once per code and reason, with the PC callback's text. The
// capture stands in for the Quest writer, which is a sentinel log record (lobby_full_hooks.cpp).
namespace declines {
struct Seen {
  std::uint8_t code;
  nevr_lobby_failure_message::Decline reason;
};
std::vector<Seen> g_seen;
void Capture(std::uint8_t code, nevr_lobby_failure_message::Decline reason) {
  g_seen.push_back(Seen{code, reason});
}
}  // namespace declines

TEST(TheDeclineLineIsThePCCallbacksTextForCodeEightWithNoMessage) {
  namespace rules = nevr_lobby_failure_message;
  char line[rules::kDeclineLineBytes];
  rules::FormatDeclineLine(line, 8, rules::Decline::kNoMessage);
  CHECK(std::string(line) ==
        "[NEVR.LOBBY] failure message not shown code=8 title=lobby full reason=no_message: "
        "the game shows its fixed literal for this code instead");
}

TEST(AnEmptyMessageOnTheErrorPageIsDeclinedOnceAndSaidOnce) {
  namespace rules = nevr_lobby_failure_message;
  namespace lf = quest_social::lobby_full;
  std::vector<std::uint8_t> game(rules::kErrorBlockOffset + 1 + rules::kErrorLines * rules::kErrorLineBytes, 0xAA);
  lf::RefusalReads reads;
  reads.game = game.data();  // the message is left empty
  rules::DeclineLog log;
  declines::g_seen.clear();
  CHECK(lf::NoteMessageNotShown(log, reads, 8, &declines::Capture));
  CHECK(!lf::NoteMessageNotShown(log, reads, 8, &declines::Capture));  // the repeat says nothing
  CHECK(declines::g_seen.size() == 1U);
  if (declines::g_seen.size() == 1U) {  // indexed only when the size holds, so a failed size check does not abort the run
    CHECK(declines::g_seen[0].code == 8);
    CHECK(declines::g_seen[0].reason == rules::Decline::kNoMessage);
  }
}

TEST(ANoGameObjectIsDeclinedAsNoGameBaseEvenWithAMessageThere) {
  namespace rules = nevr_lobby_failure_message;
  namespace lf = quest_social::lobby_full;
  lf::RefusalReads reads;  // the game object is null
  std::memcpy(reads.message, "Lobby is full, try another", sizeof "Lobby is full, try another");
  rules::DeclineLog log;
  declines::g_seen.clear();
  CHECK(lf::NoteMessageNotShown(log, reads, 8, &declines::Capture));
  CHECK(declines::g_seen.size() == 1U);
  if (declines::g_seen.size() == 1U) {
    CHECK(declines::g_seen[0].reason == rules::Decline::kNoGameBase);
  }
}

TEST(EachReasonIsSaidOnceAndAnotherReasonGetsItsOwnLine) {
  namespace rules = nevr_lobby_failure_message;
  namespace lf = quest_social::lobby_full;
  std::vector<std::uint8_t> game(rules::kErrorBlockOffset + 1 + rules::kErrorLines * rules::kErrorLineBytes, 0xAA);
  lf::RefusalReads noGame;  // no game object, empty message: no_game_base
  lf::RefusalReads noMessage;
  noMessage.game = game.data();  // a game object, empty message: no_message
  rules::DeclineLog log;
  declines::g_seen.clear();
  CHECK(lf::NoteMessageNotShown(log, noGame, 8, &declines::Capture));
  CHECK(lf::NoteMessageNotShown(log, noMessage, 8, &declines::Capture));
  CHECK(!lf::NoteMessageNotShown(log, noGame, 8, &declines::Capture));
  CHECK(declines::g_seen.size() == 2U);
  if (declines::g_seen.size() == 2U) {
    CHECK(declines::g_seen[0].reason == rules::Decline::kNoGameBase);
    CHECK(declines::g_seen[1].reason == rules::Decline::kNoMessage);
  }
}

TEST(AShownMessageIsNotDeclined) {
  namespace rules = nevr_lobby_failure_message;
  namespace lf = quest_social::lobby_full;
  std::vector<std::uint8_t> game(rules::kErrorBlockOffset + 1 + rules::kErrorLines * rules::kErrorLineBytes, 0xAA);
  lf::RefusalReads reads;
  reads.game = game.data();
  std::memcpy(reads.message, "Lobby is full, try another", sizeof "Lobby is full, try another");
  rules::DeclineLog log;
  declines::g_seen.clear();
  CHECK(!lf::NoteMessageNotShown(log, reads, 8, &declines::Capture));
  CHECK(declines::g_seen.empty());
}

TEST(TheRefusalRecordsAreSaidOncePerCodeAndRecord) {
  namespace lf = quest_social::lobby_full;
  lf::RefusalSayings said;
  CHECK(said.FirstTime(8, lf::RefusalSayings::Record::kDroppedGateClosed));
  CHECK(!said.FirstTime(8, lf::RefusalSayings::Record::kDroppedGateClosed));
  CHECK(said.FirstTime(8, lf::RefusalSayings::Record::kUncountedNoJoin));  // another record is said too
  CHECK(!said.FirstTime(8, lf::RefusalSayings::Record::kUncountedNoJoin));
  CHECK(said.FirstTime(7, lf::RefusalSayings::Record::kDroppedGateClosed));  // another code is said too
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

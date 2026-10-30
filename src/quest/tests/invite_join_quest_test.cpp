// #966 S4: the Quest join (src/quest/invite/join_game_quest.cpp) against a fake game block laid out at the Quest offsets. Built
// as the sentinel builds it (no exceptions). The service's two symbols are stubbed here: the join only takes answers and asks
// whether the service is on; the service itself is tested by its own host tests.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "quest/invite/frame_dispatch.h"
#include "quest/invite/join_game_quest.h"
#include "runtime/patch/invite_join.h"
#include "runtime/patch/invite_match_api.h"

namespace nevr_invite_match {
// The join's frame side: the service is on and no resolved match is waiting (the applier is not exercised here).
bool ServiceActive() noexcept { return true; }
bool TakeResolvedMatch(char*, std::size_t, std::size_t* length) noexcept {
  *length = 0;
  return false;
}
}  // namespace nevr_invite_match

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

constexpr const char* kMatch = "0123abcd-4567-89ef-0123-456789abcdef";

// The Windows GUID bytes of kMatch: Data1, Data2, Data3 little-endian, then Data4 as written.
const std::uint8_t kGuid[16] = {0xcd, 0xab, 0x23, 0x01, 0x67, 0x45, 0xef, 0x89, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

struct FakeGame {
  alignas(8) unsigned char bytes[0x9000];
};

// The fake CR15NetGame: state at +0, game object at +8.
struct FakeNetGame {
  alignas(8) unsigned char bytes[16];
};

class FakeAccess final : public nevr_quest_invite_join::GameAccess {
 public:
  bool onThread = true;
  bool prologues = true;
  bool hasNetGame = true;
  std::int32_t state = 3;
  std::int64_t now = 0;
  int enables = 0;
  int begins = 0;
  std::uint8_t lobbyAtBegin[16] = {};
  std::uint16_t teamAtBegin = 0;
  std::vector<std::string> lines;
  std::vector<std::string> calls;  // the order of the game calls: "enable", "begin"
  int netCalls = 0;
  int nullGameAtNetCall = 0;  // 0: never; n: the game pointer is cleared on the n-th NetGame() call
  FakeGame game;
  FakeNetGame net;

  FakeAccess() {
    std::memset(&game, 0, sizeof(game));
    std::memset(&net, 0, sizeof(net));
    void* gamePtr = &game;
    std::memcpy(net.bytes + 8, &gamePtr, sizeof(gamePtr));
    std::memcpy(net.bytes, &state, sizeof(state));
  }

  void SetState(std::int32_t s) {
    state = s;
    std::memcpy(net.bytes, &state, sizeof(state));
  }

  void ClearGame() {
    void* none = nullptr;
    std::memcpy(net.bytes + 8, &none, sizeof(none));
  }

  bool OnTickThread() override { return onThread; }
  void* NetGame() override {
    ++netCalls;
    if (nullGameAtNetCall != 0 && netCalls == nullGameAtNetCall) ClearGame();
    return hasNetGame ? static_cast<void*>(net.bytes) : nullptr;
  }
  bool ProloguesValid() override { return prologues; }
  void EnableMenu(void*) override {
    ++enables;
    calls.push_back("enable");
  }
  void Begin(void*) override {
    ++begins;
    calls.push_back("begin");
    std::memcpy(lobbyAtBegin, game.bytes + 0x8538, sizeof(lobbyAtBegin));
    std::memcpy(&teamAtBegin, game.bytes + 0x8558, sizeof(teamAtBegin));
  }
  void Log(nevr_invite::LogLevel, const std::string& line) override { lines.push_back(line); }
  std::int64_t NowMs() override { return now; }
};

bool Contains(const std::vector<std::string>& lines, const char* text) {
  for (const std::string& line : lines) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}

bool AllZero(const unsigned char* at, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    if (at[i] != 0) return false;
  }
  return true;
}

// A staging memory that drops writes at one offset, to show the write-back of a stage that does not read back.
class DroppingMemory final : public nevr_invite_join::StagingMemory {
 public:
  DroppingMemory(unsigned char* bytes, std::uintptr_t dropAt) : bytes_(bytes), dropAt_(dropAt) {}
  bool Read(std::uintptr_t offset, void* out, std::size_t size) override {
    std::memcpy(out, bytes_ + offset, size);
    return true;
  }
  bool Write(std::uintptr_t offset, const void* in, std::size_t size) override {
    if (offset != dropAt_) std::memcpy(bytes_ + offset, in, size);
    return true;
  }

 private:
  unsigned char* bytes_;
  std::uintptr_t dropAt_;
};

void TestStagesBeforeBegin() {
  FakeAccess access;
  nevr_quest_invite_join::Offerer offerer(access);
  const nevr_invite_join::Reason reason = offerer.Offer(kMatch, true);
  CHECK(reason == nevr_invite_join::Reason::None);
  CHECK(access.begins == 1);
  CHECK(access.enables == 1);
  CHECK(std::memcmp(access.lobbyAtBegin, kGuid, sizeof(kGuid)) == 0);
  CHECK(access.teamAtBegin == 0xFFFF);
  CHECK(Contains(access.lines, "join started route=stage lobby=0123ABCD-4567-89EF-0123-456789ABCDEF state_from=3"));
}

void TestQuestOffsetsOnly() {
  // The PC offsets (0x8540 staged lobby, 0x8560 team) stay untouched by a Quest join: a build that used them would show here.
  FakeAccess access;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::None);
  // 0x8540 is inside the Quest GUID (0x8538..0x8547), so the lobby is checked by TestStagesBeforeBegin, not here.
  CHECK(AllZero(access.game.bytes + 0x8560, 2));
  CHECK(nevr_invite_join::kQuestLayout.staged_lobby == 0x8538);
  CHECK(nevr_invite_join::kQuestLayout.staged_team == 0x8558);
  CHECK(nevr_invite_join::kQuestLayout.route_flags == 0x8518);
  CHECK(nevr_invite_join::kQuestLayout.route_slot == 0x8520);
  CHECK(nevr_invite_join::kQuestLayout.route_lobby == 0x8528);
  CHECK(nevr_invite_join::kPcLayout.staged_lobby == 0x8540);
}

void TestNotAtMenu() {
  FakeAccess access;
  access.SetState(2);  // LoggingIn
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::NotAtMenu);
  CHECK(access.begins == 0);
  CHECK(AllZero(access.game.bytes + 0x8538, 16));
  CHECK(Contains(access.lines, "join not started reason=not_at_menu"));
}

void TestNoNetGame() {
  FakeAccess access;
  access.hasNetGame = false;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::NotAtMenu);
  CHECK(Contains(access.lines, "state_from=none"));
  CHECK(access.begins == 0);
}

void TestRepeatInsideWindowIsBusy() {
  FakeAccess access;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::None);
  access.now = 1000;  // inside the 15 s window
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::Busy);
  CHECK(access.begins == 1);
  access.now = nevr_invite_join::kJoinWindowMs + 1;  // after the window: a new join may begin
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::None);
  CHECK(access.begins == 2);
}

void TestPrologueMismatchWritesNothing() {
  FakeAccess access;
  access.prologues = false;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::Prologue);
  CHECK(access.begins == 0);
  CHECK(AllZero(access.game.bytes + 0x8538, 16));
  CHECK(AllZero(access.game.bytes + 0x8558, 2));
}

void TestWrongThreadWritesNothing() {
  FakeAccess access;
  access.onThread = false;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::WrongThread);
  CHECK(access.begins == 0);
  CHECK(AllZero(access.game.bytes + 0x8538, 16));
}

void TestOffWritesNothing() {
  FakeAccess access;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, false) == nevr_invite_join::Reason::Off);
  CHECK(access.begins == 0);
  CHECK(AllZero(access.game.bytes + 0x8538, 16));
}

void TestOtherRouteStagesNothing() {
  FakeAccess access;
  access.game.bytes[0x8518] = 0x01;  // route flags: low nibble set
  const std::uint64_t slot = 1;      // the gate at +0x8520 is set: the game's own join would go elsewhere
  std::memcpy(access.game.bytes + 0x8520, &slot, sizeof(slot));
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::OtherRoute);
  CHECK(access.begins == 0);
  CHECK(AllZero(access.game.bytes + 0x8538, 16));
  CHECK(Contains(access.lines, "reason=other_route"));
}

void TestRouteFlagsWithoutGateStage() {
  // A route flag with its gate clear does not send the game elsewhere: the stage goes ahead.
  FakeAccess access;
  access.game.bytes[0x8518] = 0x01;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::None);
  CHECK(access.begins == 1);
}

void TestForeignLobbyAtRouteStagesNothing() {
  // A non-invalid SUuid at +0x8528 sends the game's own join to the group route: nothing staged.
  FakeAccess access;
  access.game.bytes[0x8528] = 0x42;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::OtherRoute);
  CHECK(access.begins == 0);
}

void TestWriteBackOnMismatch() {
  // A stage whose team word does not read back is put back: the prior zero is restored and the join refuses.
  FakeGame game;
  std::memset(&game, 0, sizeof(game));
  DroppingMemory memory(game.bytes, 0x8558);
  std::vector<std::string> lines;
  const auto log = [&lines](nevr_invite::LogLevel, const std::string& line) { lines.push_back(line); };
  nevr_invite_join::LobbyBytes lobby{};
  nevr_invite_join::ParseLobbyBytes(kMatch, lobby);
  const nevr_invite_join::StageOutcome outcome = nevr_invite_join::StageAndVerify(memory, lobby, 0xFFFF, log, nevr_invite_join::kQuestLayout);
  CHECK(outcome.step == nevr_invite_join::StageStep::Mismatch);
  CHECK(outcome.field == nevr_invite_join::StageField::Team);
  CHECK(AllZero(game.bytes + 0x8538, 16));
}

// The menu event runs before the begin, as the game's own main-menu join does (ReVault: EnableTransitionMenu, then
// BeginMultiplayer). Killed by a mutant that swaps the two calls.
void TestMenuEventRunsBeforeBegin() {
  FakeAccess access;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::None);
  CHECK(access.calls.size() == 2);
  if (access.calls.size() == 2) {
    CHECK(access.calls[0] == "enable");
    CHECK(access.calls[1] == "begin");
  }
}

// No game object at stage time: the stage reads nothing, names the read step and begins nothing.
void TestNoGameObjectAtStageIsAReadFailure() {
  FakeAccess access;
  access.ClearGame();
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::StageFailed);
  CHECK(access.begins == 0);
  CHECK(Contains(access.lines, "step=read field=lobby"));
}

// The game object disappears between the stage and the begin: the begin is not called, the stage is put back, and the
// refusal is NoGameObject. The third NetGame() call is the begin's own read of the game object.
void TestGameObjectGoneBeforeBeginUndoesTheStage() {
  FakeAccess access;
  access.nullGameAtNetCall = 3;
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::NoGameObject);
  CHECK(access.begins == 0);
  CHECK(Contains(access.lines, "join begin not called, no game object after the stage"));
  // The staged lobby the stage wrote must be gone again: the undo needs the game object the stage remembered.
  CHECK(AllZero(access.game.bytes + 0x8538, 16));
}

// The frame dispatcher's tick-thread test: true on the thread that last ran Tick, false on any other thread.
void TestOnTickThreadIsTheThreadThatRanTick() {
  nevr_quest::invite::frame::Tick();
  CHECK(nevr_quest::invite::frame::OnTickThread());
  bool other = true;
  std::thread worker([&other] { other = nevr_quest::invite::frame::OnTickThread(); });
  worker.join();
  CHECK(!other);
}

void TestGuidByteOrder() {
  nevr_invite_join::LobbyBytes lobby{};
  CHECK(nevr_invite_join::ParseLobbyBytes(kMatch, lobby));
  CHECK(std::memcmp(lobby.data(), kGuid, sizeof(kGuid)) == 0);
  CHECK(nevr_invite_join::LobbyText(lobby) == "0123ABCD-4567-89EF-0123-456789ABCDEF");
}

}  // namespace

int main() {
  TestStagesBeforeBegin();
  TestQuestOffsetsOnly();
  TestNotAtMenu();
  TestNoNetGame();
  TestRepeatInsideWindowIsBusy();
  TestPrologueMismatchWritesNothing();
  TestWrongThreadWritesNothing();
  TestOffWritesNothing();
  TestOtherRouteStagesNothing();
  TestRouteFlagsWithoutGateStage();
  TestForeignLobbyAtRouteStagesNothing();
  TestWriteBackOnMismatch();
  TestGuidByteOrder();
  TestMenuEventRunsBeforeBegin();
  TestNoGameObjectAtStageIsAReadFailure();
  TestGameObjectGoneBeforeBeginUndoesTheStage();
  TestOnTickThreadIsTheThreadThatRanTick();
  if (g_failures != 0) {
    std::fprintf(stderr, "invite_join_quest_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("invite_join_quest_test: all checks pass\n");
  return 0;
}

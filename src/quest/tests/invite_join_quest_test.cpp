// #966 S4: the Quest join (src/quest/invite/join_game_quest.cpp) against a fake game block laid out at the Quest offsets. Built
// as the sentinel builds it (no exceptions). The service's two symbols are stubbed here: the join only takes answers and asks
// whether the service is on; the service itself is tested by its own host tests.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>

#include "quest/invite/frame_dispatch.h"
#include "quest/invite/join_game_quest.h"
#include "quest/invite/join_log_fields.h"
#include "quest/invite/ui_text_quest.h"
#include "runtime/patch/invite_join.h"
#include "runtime/patch/invite_match_api.h"

namespace {

// The service's state and resolved matches, set by each test: the join's frame side reads them through the stubs below.
bool g_serviceActive = true;
std::deque<std::string> g_matches;
std::deque<nevr_invite_match::ShareText> g_texts;  // the texts the service's answers asked for (#1726)
int g_takeCalls = 0;

}  // namespace

namespace nevr_invite_match {
bool ServiceActive() noexcept { return g_serviceActive; }
bool ResolvedMatchPending() noexcept { return !g_matches.empty() || !g_texts.empty(); }
bool TakeShareText(ShareText* out) noexcept {
  if (out == nullptr || g_texts.empty()) return false;
  *out = g_texts.front();
  g_texts.pop_front();
  return true;
}
bool TakeResolvedMatch(char* out, std::size_t capacity, std::size_t* length) noexcept {
  ++g_takeCalls;
  if (g_matches.empty()) {
    *length = 0;
    return false;
  }
  const std::string& next = g_matches.front();
  std::size_t n = next.size();
  if (n > capacity) n = 0;
  if (n > 0) std::memcpy(out, next.data(), n);
  *length = n;
  g_matches.pop_front();
  return true;
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

  void SetGame(void* at) { std::memcpy(net.bytes + 8, &at, sizeof(at)); }

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

// The game pointer names memory no mapping holds: the join reads nothing and writes nothing, and it refuses instead of faulting.
void TestUnmappedGameObjectIsRefused() {
  void* gone = mmap(nullptr, sizeof(FakeGame), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(gone != MAP_FAILED);
  if (gone == MAP_FAILED) return;
  munmap(gone, sizeof(FakeGame));
  FakeAccess access;
  access.SetGame(gone);
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::StageFailed);
  CHECK(access.begins == 0);
}

// The game object is readable but not writable: the reads pass, the first write is refused, nothing is staged, and the join refuses.
void TestReadOnlyGameObjectWritesNothing() {
  void* readOnly = mmap(nullptr, sizeof(FakeGame), PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(readOnly != MAP_FAILED);
  if (readOnly == MAP_FAILED) return;
  FakeAccess access;
  access.SetGame(readOnly);
  nevr_quest_invite_join::Offerer offerer(access);
  CHECK(offerer.Offer(kMatch, true) == nevr_invite_join::Reason::StageFailed);
  CHECK(access.begins == 0);
  CHECK(AllZero(static_cast<const unsigned char*>(readOnly) + 0x8538, 16));
  munmap(readOnly, sizeof(FakeGame));
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

// The Quest's own order: the menu event, then the begin. This pins the code's order, not a measured game order: the order is
// not measured on Quest (the headset run is owed), and the PC join calls only the begin (src/runtime/patch/invite_join_game.cpp).
// Killed by a mutant that swaps the two calls.
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

// Resets the frame dispatcher and the service stubs for one frame test.
void ResetFrameTest() {
  nevr_quest::invite::frame::ResetForTest();
  g_matches.clear();
  g_texts.clear();
  g_takeCalls = 0;
  g_serviceActive = true;
}

// A resolved answer waiting while the service is on: the applier takes it and the join begins once.
void TestAppliedAnswerOffersTheMatch() {
  ResetFrameTest();
  FakeAccess access;
  nevr_quest_invite_join::SetFrameAccessForTest(&access);
  g_matches.push_back(kMatch);
  CHECK(nevr_quest_invite_join::RegisterAnswerApplier());
  nevr_quest::invite::frame::Tick();
  CHECK(access.begins == 1);
  CHECK(access.calls.size() == 2 && access.calls[0] == "enable" && access.calls[1] == "begin");
  CHECK(g_matches.empty());
  CHECK(Contains(access.lines, "join started route=stage"));
  nevr_quest_invite_join::SetFrameAccessForTest(nullptr);
}

// The same answer while the service is off: the applier takes it and the join refuses (feature off), so nothing is staged.
void TestAppliedAnswerWhileServiceOffIsNotOffered() {
  ResetFrameTest();
  g_serviceActive = false;
  FakeAccess access;
  access.now = 100000;  // past the join window the earlier test opened on the shared offerer: only the service state decides
  nevr_quest_invite_join::SetFrameAccessForTest(&access);
  g_matches.push_back(kMatch);
  CHECK(nevr_quest_invite_join::RegisterAnswerApplier());
  nevr_quest::invite::frame::Tick();
  CHECK(access.begins == 0);
  CHECK(g_matches.empty());
  CHECK(AllZero(access.game.bytes + 0x8538, 16));
  nevr_quest_invite_join::SetFrameAccessForTest(nullptr);
}

// An idle frame costs no take: with no resolved match waiting, the applier does not call TakeResolvedMatch (the #1415 early-out,
// as the PC applier takes it). Three frames, none of them takes.
void TestIdleFrameDoesNotTakeMatches() {
  ResetFrameTest();
  FakeAccess access;
  nevr_quest_invite_join::SetFrameAccessForTest(&access);
  CHECK(nevr_quest_invite_join::RegisterAnswerApplier());
  nevr_quest::invite::frame::Tick();
  nevr_quest::invite::frame::Tick();
  nevr_quest::invite::frame::Tick();
  CHECK(g_takeCalls == 0);
  CHECK(access.begins == 0);
  nevr_quest_invite_join::SetFrameAccessForTest(nullptr);
}

void TestGuidByteOrder() {
  nevr_invite_join::LobbyBytes lobby{};
  CHECK(nevr_invite_join::ParseLobbyBytes(kMatch, lobby));
  CHECK(std::memcmp(lobby.data(), kGuid, sizeof(kGuid)) == 0);
  CHECK(nevr_invite_join::LobbyText(lobby) == "0123ABCD-4567-89EF-0123-456789ABCDEF");
}

// #1542: the join's text becomes record fields. reason, lobby and state_from are keys, numbers are numbers, and words that
// are not key=value stay in msg.
const nevr_quest_invite_join::JoinField* FieldNamed(const nevr_quest_invite_join::JoinLine& line, const char* key) {
  for (const nevr_quest_invite_join::JoinField& f : line.fields) {
    if (f.key == key) return &f;
  }
  return nullptr;
}

void TestJoinLineSplitsIntoFields() {
  const nevr_quest_invite_join::JoinLine notStarted = nevr_quest_invite_join::SplitJoinLine(
      "[NEVR.INVITE] join not started reason=not_at_menu lobby=0123ABCD-4567-89EF-0123-456789ABCDEF state_from=2");
  CHECK(notStarted.tag == "NEVR.INVITE");
  CHECK(notStarted.msg == "join not started");
  const nevr_quest_invite_join::JoinField* reason = FieldNamed(notStarted, "reason");
  CHECK(reason != nullptr && reason->text == "not_at_menu" && !reason->number);
  const nevr_quest_invite_join::JoinField* lobby = FieldNamed(notStarted, "lobby");
  CHECK(lobby != nullptr && lobby->text == "0123ABCD-4567-89EF-0123-456789ABCDEF");
  const nevr_quest_invite_join::JoinField* stateFrom = FieldNamed(notStarted, "state_from");
  CHECK(stateFrom != nullptr && stateFrom->number && stateFrom->value == 2);

  const nevr_quest_invite_join::JoinLine none = nevr_quest_invite_join::SplitJoinLine(
      "[NEVR.INVITE] join not started reason=not_at_menu lobby=0123ABCD-4567-89EF-0123-456789ABCDEF state_from=none");
  const nevr_quest_invite_join::JoinField* noneState = FieldNamed(none, "state_from");
  CHECK(noneState != nullptr && !noneState->number && noneState->text == "none");

  const nevr_quest_invite_join::JoinLine stage = nevr_quest_invite_join::SplitJoinLine(
      "[NEVR.INVITE] join stage undo failed after begin step=unstage field=lobby");
  CHECK(stage.msg == "join stage undo failed after begin");
  CHECK(FieldNamed(stage, "step") != nullptr && FieldNamed(stage, "step")->text == "unstage");
  CHECK(FieldNamed(stage, "field") != nullptr && FieldNamed(stage, "field")->text == "lobby");

  // A token whose key is not a plain identifier is a word of msg, and a repeated key keeps its first value.
  const nevr_quest_invite_join::JoinLine odd = nevr_quest_invite_join::SplitJoinLine("[NEVR.INVITE] a.b=c c-d=e x=1 x=2");
  CHECK(odd.msg == "a.b=c c-d=e");
  CHECK(odd.fields.size() == 1 && odd.fields[0].key == "x" && odd.fields[0].value == 1);
}

}  // namespace


// ---- #1726: the share page's texts, through the shared writer with the Quest's address table ----

// One Quest gamespace as the game lays it out: [gs+0x370] -> table, [table+0] -> a name-sorted array of 16-byte entries (name,
// then the actor index as a u16 at +8), [table+0x30] -> the count.
struct FakeSpace {
  alignas(8) unsigned char space[0x400] = {};
  alignas(8) unsigned char table[0x100] = {};
  std::vector<unsigned char> entries;
  void Set(const std::vector<std::pair<std::uint64_t, std::uint16_t>>& list) {
    entries.assign(list.size() * 16, 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
      std::memcpy(entries.data() + i * 16, &list[i].first, 8);
      std::memcpy(entries.data() + i * 16 + 8, &list[i].second, 2);
    }
    const std::uint64_t t = reinterpret_cast<std::uintptr_t>(table);
    const std::uint64_t e = reinterpret_cast<std::uintptr_t>(entries.data());
    const std::uint64_t n = list.size();
    std::memcpy(space + 0x370, &t, 8);
    std::memcpy(table + 0x00, &e, 8);
    std::memcpy(table + 0x30, &n, 8);
  }
};

class FakeCalls final : public nevr_quest::ui_text::GameCalls {
 public:
  const char* states[3] = {"ok", "ok", "ok"};
  std::uintptr_t game = 0x1000;
  FakeSpace* space = nullptr;
  bool canvas = true;
  int pins = 0;
  int gameSpaceCalls = 0;
  std::uint64_t lastIdent = 0;
  std::uint64_t lastSymbol = 0;
  std::vector<std::pair<std::uint64_t, std::string>> written;  // element, text
  std::vector<std::string> lines;
  std::uint64_t now = 1000;

  bool Pin(const char* (&state)[3]) override {
    ++pins;
    for (int i = 0; i < 3; ++i) state[i] = states[i];
    return std::strcmp(states[0], "ok") == 0 && std::strcmp(states[1], "ok") == 0 && std::strcmp(states[2], "ok") == 0;
  }
  std::uintptr_t Game() override { return game; }
  std::uintptr_t GameSpace(std::uintptr_t, std::uint64_t index) override {
    ++gameSpaceCalls;
    return index == 0 && space != nullptr ? reinterpret_cast<std::uintptr_t>(space->space) : 0;
  }
  bool Read(std::uintptr_t address, void* out, std::size_t length) override {
    std::memcpy(out, reinterpret_cast<const void*>(address), length);
    return true;
  }
  bool FindCanvas(std::uintptr_t, std::uint64_t ident, std::uintptr_t* canvasOut, std::uint32_t* handle) override {
    lastIdent = ident;
    if (!canvas) return false;
    *canvasOut = 0xc0ffee;
    *handle = 7;
    return true;
  }
  bool SetText(std::uintptr_t, std::uint32_t, std::uint64_t element, const char* text) override {
    written.emplace_back(element, text);
    return true;
  }
  std::uint64_t NowMs() override { return now; }
  std::string FirstWriteLine() override { return "[NEVR.UI] text first write on_game_thread=true"; }
  void Log(nevr_ui_text::LogLevel, const std::string& line) override { lines.push_back(line); }
};

nevr_invite_match::ShareText MakeText(std::uint64_t actor, std::uint64_t element, const char* field, const char* text) {
  nevr_invite_match::ShareText t;
  t.actor = actor;
  t.element = element;
  std::strncpy(t.field, field, sizeof(t.field) - 1);
  t.length = std::strlen(text);
  std::memcpy(t.text, text, t.length);
  return t;
}

constexpr std::uint64_t kLinkActor = 0xc3f9f167b2a06043ULL;
constexpr std::uint64_t kLinkElement = 0x3657cd7405999810ULL;

// A created code's three texts reach the writer on the next frame, and the link is written as `<coming soon>` into the link
// actor's canvas element, the actor found in the Quest's name-sorted table (signed order: 0xa9.. < 0xc3.. < 0x0e.. as int64).
void TestCreatedCodeWritesTheComingSoonLink() {
  ResetFrameTest();
  FakeAccess access;
  FakeCalls calls;
  FakeSpace space;
  space.Set({{0xa9bd0cb4e6f2004cULL, 3}, {0xc3f9f167b2a06043ULL, 11}, {0x0ec8e16986ef88c8ULL, 12}});
  calls.space = &space;
  nevr_quest::ui_text::ResetForTest(&calls);
  nevr_quest_invite_join::SetFrameAccessForTest(&access);
  g_texts.push_back(MakeText(0xa9bd0cb4e6f2004cULL, 0x35e290123dfa9c72ULL, "code", "K7M2Q"));
  g_texts.push_back(MakeText(0x0ec8e16986ef88c8ULL, 0xd38c12d22d3552ebULL, "expires", "EXPIRES: 05:38 UTC"));
  g_texts.push_back(MakeText(kLinkActor, kLinkElement, "link", "<coming soon>"));
  CHECK(nevr_quest_invite_join::RegisterAnswerApplier());
  nevr_quest::invite::frame::Tick();
  CHECK(g_texts.empty());
  CHECK(calls.written.size() == 3);
  bool linkWritten = false;
  for (const auto& w : calls.written) linkWritten = linkWritten || (w.first == kLinkElement && w.second == "<coming soon>");
  CHECK(linkWritten);
  CHECK(calls.lastIdent == (0x0000ffffffff0000ULL | 11));  // the last write was the link actor, index 11 in the table
  CHECK(Contains(access.lines, "[NEVR.INVITE] text posted field=link bytes=13"));
  CHECK(Contains(access.lines, "[NEVR.INVITE] text posted field=code bytes=5"));
  CHECK(Contains(calls.lines, "[NEVR.UI] text writer ready game_space=ok find_canvas=ok set_text=ok"));
  CHECK(Contains(calls.lines, "[NEVR.UI] text written actor=0xc3f9f167b2a06043 element=0x3657cd7405999810 bytes=13 attempts=1"));
  CHECK(!Contains(calls.lines, "K7M2Q") && !Contains(access.lines, "K7M2Q"));  // a code is never logged
  nevr_quest_invite_join::SetFrameAccessForTest(nullptr);
  nevr_quest::ui_text::ResetForTest(nullptr);
}

// A libr15 whose pinned words differ turns the writer off: nothing is called, each text says writer_off and the off line names
// the call that did not match.
void TestWriterOffWhenAPinMismatches() {
  ResetFrameTest();
  FakeAccess access;
  FakeCalls calls;
  calls.states[1] = "prologue_mismatch";
  nevr_quest::ui_text::ResetForTest(&calls);
  nevr_quest_invite_join::SetFrameAccessForTest(&access);
  g_texts.push_back(MakeText(kLinkActor, kLinkElement, "link", "<coming soon>"));
  g_texts.push_back(MakeText(0xa9bd0cb4e6f2004cULL, 0x35e290123dfa9c72ULL, "code", "K7M2Q"));
  CHECK(nevr_quest_invite_join::RegisterAnswerApplier());
  nevr_quest::invite::frame::Tick();
  CHECK(calls.written.empty());
  CHECK(calls.gameSpaceCalls == 0);
  CHECK(calls.pins == 1);  // the pins are checked once; the second text meets the same verdict
  CHECK(Contains(access.lines, "[NEVR.INVITE] text not posted field=link bytes=13 reason=writer_off"));
  CHECK(Contains(access.lines, "[NEVR.INVITE] text not posted field=code bytes=5 reason=writer_off"));
  CHECK(Contains(calls.lines, "[NEVR.UI] text writer off game_space=ok find_canvas=prologue_mismatch set_text=ok"));
  nevr_quest_invite_join::SetFrameAccessForTest(nullptr);
  nevr_quest::ui_text::ResetForTest(nullptr);
}

// A frame with no text waiting and nothing ever posted never touches the game or the pins.
void TestIdleFrameTouchesNoTextCalls() {
  ResetFrameTest();
  FakeAccess access;
  FakeCalls calls;
  nevr_quest::ui_text::ResetForTest(&calls);
  nevr_quest_invite_join::SetFrameAccessForTest(&access);
  CHECK(nevr_quest_invite_join::RegisterAnswerApplier());
  nevr_quest::invite::frame::Tick();
  nevr_quest::invite::frame::Tick();
  CHECK(calls.pins == 0 && calls.gameSpaceCalls == 0 && calls.lines.empty());
  nevr_quest_invite_join::SetFrameAccessForTest(nullptr);
  nevr_quest::ui_text::ResetForTest(nullptr);
}

// A page that does not hold the actor yet is tried again on later frames and explains the miss once; when the actor appears it is
// written. The wait is the writer's own (nevr_ui_text::Pending), not a Quest copy.
void TestAnActorThatIsNotThereYetIsRetriedThenWritten() {
  ResetFrameTest();
  FakeAccess access;
  FakeCalls calls;
  FakeSpace space;
  space.Set({{0x10, 1}});
  calls.space = &space;
  nevr_quest::ui_text::ResetForTest(&calls);
  nevr_quest_invite_join::SetFrameAccessForTest(&access);
  g_texts.push_back(MakeText(kLinkActor, kLinkElement, "link", "<coming soon>"));
  CHECK(nevr_quest_invite_join::RegisterAnswerApplier());
  nevr_quest::invite::frame::Tick();
  CHECK(calls.written.empty());
  CHECK(Contains(calls.lines, "[NEVR.UI] text waiting actor=0xc3f9f167b2a06043 element=0x3657cd7405999810 bytes=13 attempts=1 outcome=no_actor"));
  CHECK(Contains(calls.lines, "[NEVR.UI] text probe space=0 gs=0x"));
  space.Set({{kLinkActor, 11}, {0x10, 1}});  // kLinkActor is negative as int64: it sorts before 0x10
  calls.now += 100;
  nevr_quest::invite::frame::Tick();
  CHECK(calls.written.size() == 1 && calls.written[0].second == "<coming soon>");
  CHECK(Contains(calls.lines, "bytes=13 attempts=2"));
  nevr_quest_invite_join::SetFrameAccessForTest(nullptr);
  nevr_quest::ui_text::ResetForTest(nullptr);
}

int main() {
  TestStagesBeforeBegin();
  TestQuestOffsetsOnly();
  TestNotAtMenu();
  TestNoNetGame();
  TestUnmappedGameObjectIsRefused();
  TestReadOnlyGameObjectWritesNothing();
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
  TestAppliedAnswerOffersTheMatch();
  TestAppliedAnswerWhileServiceOffIsNotOffered();
  TestIdleFrameDoesNotTakeMatches();
  TestJoinLineSplitsIntoFields();
  TestCreatedCodeWritesTheComingSoonLink();
  TestWriterOffWhenAPinMismatches();
  TestIdleFrameTouchesNoTextCalls();
  TestAnActorThatIsNotThereYetIsRetriedThenWritten();
  if (g_failures != 0) {
    std::fprintf(stderr, "invite_join_quest_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("invite_join_quest_test: all checks pass\n");
  return 0;
}

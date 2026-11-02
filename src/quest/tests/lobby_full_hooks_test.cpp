// Host test for the lobby-full hooks' handlers (quest/social/lobby_full_hooks.cpp, #1160). The translation unit under test is
// included, so the test reaches the handlers OnJoin and OnFailure themselves (the same functions the thunks call) with a fake
// lobby client, a fake failure message and a fake `original` that does nothing. What it does not run: the thunk entries and
// the trampolines, which execute AArch64 (row_hooks_install_test installs them, on a synthetic image).
//
// Run: lobby_full_hooks_test [name-filter]

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mini_test.h"
#include "quest/social/lobby_full_hooks.cpp"  // NOLINT: the handlers are in its anonymous namespace
#include "runtime/log/symcache.h"

// The generated symbol cache is linked into the sentinel; these tests never look a name up.
namespace evr {
extern const SymCacheEntry kSymCacheData[] = {{1, "stub"}};
extern const int kSymCacheCount = 1;
}  // namespace evr

namespace {

namespace rules = nevr_lobby_failure_message;
using namespace quest_social::lobby_full;

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void CaptureLine(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }

struct Capture {
  sentinel::LogSink previous;
  Capture() : previous(sentinel::SetLogSink(&CaptureLine)) { g_lines.clear(); }
  ~Capture() { sentinel::SetLogSink(previous); }
};

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// The first captured line holding every part, or nullptr.
const Line* Find(std::initializer_list<const char*> parts) {
  for (const Line& l : g_lines) {
    bool all = true;
    for (const char* p : parts) all = all && Has(l.text, p);
    if (all) return &l;
  }
  return nullptr;
}

int g_originalCalls = 0;
void FakeJoin(void*, const void*, const void*) { ++g_originalCalls; }
void FakeFailure(void*, const void*) { ++g_originalCalls; }

constexpr std::size_t kGameSize = rules::kErrorBlockOffset + 1 + rules::kErrorLines * rules::kErrorLineBytes;

// A lobby client whose +8 holds `game`, +0x910 holds `other` and +0x448 holds `status`.
struct Rig {
  std::vector<std::uint8_t> game = std::vector<std::uint8_t>(kGameSize, 0xAA);
  std::vector<std::uint8_t> other = std::vector<std::uint8_t>(kGameSize, 0xBB);
  std::vector<std::uint8_t> client = std::vector<std::uint8_t>(0x1000, 0);
  std::vector<std::uint8_t> data = std::vector<std::uint8_t>(0x60, 0);  // data + 0x18 code, data + 0x20 message
  std::vector<std::uint8_t> failure = std::vector<std::uint8_t>(0x20, 0);
  std::vector<std::uint8_t> join = std::vector<std::uint8_t>(0x30, 0);  // msg + 0x18 key

  Rig(std::int32_t status, std::uint8_t seed) {
    std::uint8_t* gamePtr = game.data();
    std::uint8_t* otherPtr = other.data();
    std::memcpy(client.data() + 0x8, &gamePtr, sizeof gamePtr);
    std::memcpy(client.data() + 0x910, &otherPtr, sizeof otherPtr);
    std::memcpy(client.data() + 0x448, &status, sizeof status);
    data[0x18] = 8;
    std::memcpy(data.data() + 0x20, "Lobby is full, try another", sizeof "Lobby is full, try another");
    const std::uint8_t* dataPtr = data.data();
    std::memcpy(failure.data() + 0x10, &dataPtr, sizeof dataPtr);
    for (std::size_t i = 0; i < 16; ++i) join[0x18 + i] = static_cast<std::uint8_t>(seed + i);
  }
  void* Self() { return client.data(); }
  bool GameUntouched() const { return game == std::vector<std::uint8_t>(kGameSize, 0xAA); }
  bool OtherUntouched() const { return other == std::vector<std::uint8_t>(kGameSize, 0xBB); }
};

TEST(ALobbyFullRefusalWritesTheErrorPageIntoTheGameAtPlusEightAndLogsMessageShown) {
  Capture capture;
  Rig rig(5, 0x21);  // status 5 is inside the game's gate (status - 4 < 3)
  g_originalCalls = 0;
  OnJoin(&FakeJoin, rig.Self(), rig.join.data(), nullptr);  // records the lobby, so the cooldown path runs
  CHECK_EQ(g_originalCalls, 1);
  OnFailure(&FakeFailure, rig.Self(), rig.failure.data());
  CHECK_EQ(g_originalCalls, 2);  // the game's own callback still ran

  const std::uint8_t* block = rig.game.data() + rules::kErrorBlockOffset;
  CHECK_EQ(block[0], rules::kErrorFlagFourLines);
  CHECK(std::string(reinterpret_cast<const char*>(block + 1)) == "lobby full");
  CHECK(std::string(reinterpret_cast<const char*>(block + 1 + rules::kErrorLineBytes)) == "Lobby is full, try another");
  CHECK(rig.OtherUntouched());

  const Line* shown = Find({"\"event\":\"lobby_full\"", "\"status\":\"message_shown\""});
  CHECK(shown != nullptr);
  if (shown != nullptr) {
    CHECK(shown->level == sentinel::LogLevel::kInfo);
    CHECK(Has(shown->text, "\"code\":8"));
    CHECK(Has(shown->text, "\"lobby\":\"0x2122232425262728292a2b2c2d2e2f30\""));
    CHECK(Has(shown->text, "\"message_bytes\":26"));
  }
  const Line* wait = Find({"\"status\":\"wait\"", "\"lobby\":\"0x2122232425262728292a2b2c2d2e2f30\""});
  CHECK(wait != nullptr);
}

TEST(ARefusalWhileTheStatusIsOutOfRangeWritesNothingAndLogsTheDroppedReason) {
  for (const std::int32_t status : {0, 3, 7, 9}) {
    Capture capture;
    Rig rig(status, 0x41);
    g_originalCalls = 0;
    OnJoin(&FakeJoin, rig.Self(), rig.join.data(), nullptr);
    OnFailure(&FakeFailure, rig.Self(), rig.failure.data());
    CHECK_EQ(g_originalCalls, 2);
    CHECK(rig.GameUntouched());
    CHECK(rig.OtherUntouched());
    const Line* dropped = Find({"\"status\":\"refusal_dropped\"", "\"reason\":\"status_gate_closed\""});
    CHECK(dropped != nullptr);
    if (dropped != nullptr) CHECK(dropped->level == sentinel::LogLevel::kDebug);
    CHECK(Find({"\"status\":\"message_shown\""}) == nullptr);
    CHECK(Find({"\"status\":\"wait\""}) == nullptr);  // not counted either
  }
}

TEST(AnotherFailureCodeIsPassedThroughUntouched) {
  Capture capture;
  Rig rig(5, 0x61);
  rig.data[0x18] = 7;
  g_originalCalls = 0;
  OnFailure(&FakeFailure, rig.Self(), rig.failure.data());
  CHECK_EQ(g_originalCalls, 1);
  CHECK(rig.GameUntouched());
  CHECK(rig.OtherUntouched());
  CHECK(g_lines.empty());
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

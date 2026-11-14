// The Quest's build of the session pin's pure part (core/map_session_pin.h, ADR 0048 decision 9), on the host: the same vectors the
// core GTest holds (the level names and the pin value the Quest's own maps were tested against), the decision row a Quest client
// meets (no local source), the view's replacement, the empty-state gate, and the login entries. The Quest compiles the same core
// source the PC does; this file is the Quest's own check that it builds and decides the same on the NDK-free host.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/map_session_pin.h"
#include "nevr/symbol_hash.h"
#include "quest/tests/test_check.h"

namespace {

namespace sp = nevr_map_session_pin;

const std::string kSourceA = "0123456789abcdef" + std::string(48, 'a');

sp::MapPin Pin(const std::string& id, const std::string& version, const std::string& platform) {
  sp::MapPin pin;
  pin.id = id;
  pin.version = version;
  pin.source_sha256 = kSourceA;
  pin.index_sha256 = std::string(64, platform == "android-arm64" ? '2' : '1');
  pin.platform = platform;
  pin.level = EchoVR::CSymbol64Hash(id.c_str());
  return pin;
}

constexpr std::uint64_t kStockLevel = 0x43e2da7914642604ull;  // mpl_combat_dyson
constexpr std::uint64_t kPinOfNothing = 0x55;

// The game half of the tick, counting every call into the game (the Quest's own GameAccess is libr15's and is not built here).
class CountingGame final : public sp::GameAccess {
 public:
  bool callable = true;
  std::uint64_t lobby = 0x7000;
  std::int32_t state = 9;
  std::uint64_t owner_level = 0;
  std::uint64_t owner_pin = 0;
  int game_calls = 0;
  int schedule_calls = 0;
  int skews = 0;
  bool Callable() override { return callable; }
  bool IsServer() override {
    ++game_calls;
    return false;
  }
  std::uint64_t Lobby() override {
    ++game_calls;
    return lobby;
  }
  std::int32_t NetState() override {
    ++game_calls;
    return state;
  }
  std::uint64_t OwnerSymbol(std::uint64_t, const char* key) override {
    ++game_calls;
    return std::string(key) == sp::kOwnerPinKey ? owner_pin : owner_level;
  }
  void SetOwnerSymbol(std::uint64_t, const char*, std::uint64_t) override { ++game_calls; }
  std::uint64_t MainLevel() override {
    ++game_calls;
    return 0;
  }
  bool WriteNotice(const char*, const char*) override {
    ++game_calls;
    return true;
  }
  bool ScheduleLeave() override {
    ++game_calls;
    ++schedule_calls;
    return true;
  }
  std::uint64_t NowMs() override { return 0; }
  void Event(const sp::PinEvent& event) override {
    if (event.kind == sp::PinEventKind::kSkew) ++skews;
  }
};

void TheVectorsOfTheQuestsOwnMaps() {
  QCHECK(sp::SessionLevelOf("mpl_lobby_b2") == 0xd09afd15b1c75c04ULL);
  QCHECK(sp::LowspecLevelOf("mpl_lobby_b2") == 0x05361c73bb73db19ULL);
  QCHECK(sp::LowspecLevelOf("mpl_arena_a") == 0x7691d78d414c5701ULL);
  const sp::MapPin p = Pin("arena2", "1.0", "windows-x86_64");
  QCHECK(sp::PinValue(p) == EchoVR::CSymbol64Hash("arena2@1.0@0123456789abcdef"));
  QCHECK(sp::PinValue(p) == sp::PinValue(Pin("arena2", "1.0", "android-arm64")));
}

void TheDecisionRowsAOnQuestClient() {
  sp::ResetForTest();
  sp::ChannelView view;
  view.listed = {{sp::SessionLevelOf("arena2"), "arena2"}, {sp::LowspecLevelOf("arena2"), "arena2"}};
  view.staged = {Pin("arena2", "1.0", "android-arm64")};
  sp::SetChannelView(view);
  const std::uint64_t good = sp::PinValue(Pin("arena2", "1.0", "android-arm64"));
  QCHECK(sp::DecideClient(0, good).action == sp::ClientAction::kNone);
  QCHECK(sp::DecideClient(kStockLevel, 7).action == sp::ClientAction::kNone);
  QCHECK(sp::DecideClient(sp::SessionLevelOf("arena2"), 0).reason == "map_unpinned");
  QCHECK(sp::DecideClient(sp::SessionLevelOf("arena2"), good).action == sp::ClientAction::kNone);
  QCHECK(sp::DecideClient(sp::LowspecLevelOf("arena2"), good).action == sp::ClientAction::kNone);  // the Quest's own level name
  const sp::ClientDecision version = sp::DecideClient(sp::SessionLevelOf("arena2"), good + 1);
  QCHECK(version.action == sp::ClientAction::kLeave && version.reason == "version" && version.request_fetch);
  QCHECK(version.notice_line == "Needs arena2 (not your 1.0); updating for next launch.");
  // A local copy never happens on the Quest: the view lists none, so the mismatch leaves the same way.
  QCHECK(sp::DecideClient(sp::SessionLevelOf("arena2"), good + 1).action == sp::ClientAction::kLeave);
  sp::ResetForTest();
}

void TheViewIsReplacedNotAccumulated() {
  sp::ResetForTest();
  sp::ChannelView first;
  first.listed = {{sp::SessionLevelOf("arena2"), "arena2"}};
  sp::SetChannelView(first);
  sp::ChannelView second;
  second.listed = {{sp::SessionLevelOf("dock"), "dock"}};
  sp::SetChannelView(second);
  QCHECK(sp::DecideClient(sp::SessionLevelOf("arena2"), 7).action == sp::ClientAction::kNone);
  QCHECK(sp::DecideClient(sp::SessionLevelOf("dock"), 7).action == sp::ClientAction::kLeave);
  sp::SetMountedPins({Pin("arena2", "1.0", "android-arm64")});
  sp::SetMountedPins({});
  QCHECK(sp::MountedPins().empty());
  sp::ResetForTest();
}

void TheEmptyStateMakesNoGameCall() {
  sp::ResetForTest();
  CountingGame game;
  game.owner_level = sp::SessionLevelOf("arena2");
  game.owner_pin = kPinOfNothing;
  for (int i = 0; i < 20; ++i) sp::TickWith(game);
  QCHECK(game.game_calls == 0);
  QCHECK(game.schedule_calls == 0);
}

void ALeaveIsDecidedOnceAndScheduled() {
  sp::ResetForTest();
  sp::ChannelView view;
  view.listed = {{sp::SessionLevelOf("arena2"), "arena2"}};
  view.staged = {Pin("arena2", "1.0", "android-arm64")};
  sp::SetChannelView(view);
  CountingGame game;
  game.owner_level = sp::SessionLevelOf("arena2");
  game.owner_pin = sp::PinValue(Pin("arena2", "1.1", "android-arm64"));
  for (int i = 0; i < 10; ++i) sp::TickWith(game);
  QCHECK(game.skews == 1);
  QCHECK(game.schedule_calls == 1);
  sp::ResetForTest();
}

void TheLoginCarriesTheEntriesWithAMountedFlag() {
  sp::ResetForTest();
  QCHECK(sp::LoginEntries().empty());
  sp::ChannelView view;
  view.staged = {Pin("arena2", "1.0", "android-arm64"), Pin("dock", "2.0", "android-arm64")};
  sp::SetChannelView(view);
  sp::SetMountedPins({Pin("arena2", "1.0", "android-arm64")});
  const nlohmann::json entries = sp::LoginEntries();
  QCHECK(entries.size() == 2);
  QCHECK(entries[0]["mounted"] == true && entries[1]["mounted"] == false);
  // The level is "0x" and sixteen lowercase hex digits, the login's spelling of rad_hash(<id>).
  char level[19];
  std::snprintf(level, sizeof(level), "0x%016llx", static_cast<unsigned long long>(sp::SessionLevelOf("arena2")));
  QCHECK(entries[0]["level"] == std::string(level));
  sp::ResetForTest();
}

}  // namespace

int main() {
  TheVectorsOfTheQuestsOwnMaps();
  TheDecisionRowsAOnQuestClient();
  TheViewIsReplacedNotAccumulated();
  TheEmptyStateMakesNoGameCall();
  ALeaveIsDecidedOnceAndScheduled();
  TheLoginCarriesTheEntriesWithAMountedFlag();
  return quest_test::Failures() == 0 ? 0 : 1;
}

#include "quest/combat/music.h"

#include <time.h>

#include <array>
#include <atomic>
#include <cstring>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/music_targets.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::music {

namespace {

namespace tgt = targets;

// ---- the bank, as a table the handler searches ------------------------------------------------------------------------------------

struct BankEntry {
  std::uint32_t id;
  Kind kind;
};
constexpr std::size_t kBankSize = 2 + 2 + 20;

// The three lists, sorted by id into one table (insertion sort at compile time), so a post that is not in the bank costs a binary search.
constexpr std::array<BankEntry, kBankSize> BuildBank() {
  std::array<BankEntry, kBankSize> table{};
  std::size_t n = 0;
  const auto add = [&](std::uint32_t id, Kind kind) {
    std::size_t i = n++;
    while (i > 0 && table[i - 1].id > id) {
      table[i] = table[i - 1];
      --i;
    }
    table[i] = BankEntry{id, kind};
  };
  for (const std::uint32_t id : kPlay) add(id, Kind::kPlay);
  for (const std::uint32_t id : kStop) add(id, Kind::kStop);
  for (const std::uint32_t id : kState) add(id, Kind::kState);
  return table;
}
constexpr std::array<BankEntry, kBankSize> kBank = BuildBank();

constexpr bool BankIsStrictlySorted() {
  for (std::size_t i = 1; i < kBank.size(); ++i) {
    if (kBank[i - 1].id >= kBank[i].id) return false;  // a repeated id would be two kinds for one event
  }
  return true;
}
static_assert(BankIsStrictlySorted(), "the combat music bank lists an event twice");

// ---- state -----------------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kNoObject = 0xFFFFFFFFu;  // "no previous Play"

// The tick publishes the game so the handler can ask the level on a Play; 0 until the first frame.
std::atomic<std::uintptr_t> g_game{0};
std::atomic<std::uintptr_t> g_self{0};  // the CWwiseEngine of the last Play (PostSoundEvent's x0)
std::atomic<std::uint32_t> g_lastObject{kNoObject};
std::atomic<bool> g_playing{false};  // a Play whose Stops are owed when the combat map goes
std::atomic<bool> g_on{false};
std::atomic<bool> g_tickRegistered{false};
std::atomic<bool> g_firstSaid{false};
std::atomic<std::uint32_t> g_playLines{0};
std::atomic<std::uint64_t> g_bankPosts{0}, g_plays{0}, g_playsActed{0}, g_playsOutOfScope{0}, g_stopsPosted{0}, g_tickStops{0};
std::atomic<std::int64_t> g_nextLookMs{0};  // the game thread only (the tick); atomic for the host test's reads
std::atomic<std::int64_t (*)() noexcept> g_clock{nullptr};

sentinel::GotHook g_hook;

std::int64_t NowMs() noexcept {
  const auto clock = g_clock.load(std::memory_order_acquire);
  if (clock != nullptr) return clock();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// One extra Stop through the game's own PostSoundEvent: PostEvent(stop, object, flags 0, no callback, no cookie), seek 0.
void PostStop(tgt::Thunk::Fn original, void* self, std::uint32_t stop, std::uint32_t object) noexcept {
  std::uint32_t flags = 0;
  std::uint32_t playingId = 0;
  g_stopsPosted.fetch_add(1, std::memory_order_relaxed);
  original(self, stop, object, &flags, nullptr, nullptr, 0, &playingId);
}

// Both music Stops on `object`.
std::uint32_t PostBothStops(tgt::Thunk::Fn original, void* self, std::uint32_t object) noexcept {
  for (const std::uint32_t stop : kStop) PostStop(original, self, stop, object);
  return 2;
}

// A direct ask of combat_level.h, not combat_scope.h, by decision: the combat level answer is cached from the loaded tables as of its last look, and
// this ask is made once per Play with the init-fiber table read as well, which is the point (a map still building is the map whose music it is).
bool InCombatMapNow() noexcept {
  const std::uintptr_t game = g_game.load(std::memory_order_acquire);
  if (game == 0) return false;  // no frame has run yet: the level is unknown, so the Play goes through untouched
  std::uint64_t levelId = 0;
  // inFlight: a Play posted while its combat map is still building (it is in the init-fiber table, not yet in the loaded tables) is the map's Play, also in a
  // map-to-map change, where the previous map is still loaded and the unload tick has not fired (upstream posts the Stops on every Play, so it has no such window).
  return level::Classify(game, /*lobby=*/false, &levelId, /*inFlight=*/true) == level::Kind::kCombatOrLobby;
}

const char* KindName(Kind kind) noexcept { return kind == Kind::kPlay ? "play" : kind == Kind::kStop ? "stop" : "state"; }

void SayFirst(Kind kind, std::uint32_t event, std::uint32_t object, bool inCombatMap) noexcept {
  if (g_firstSaid.exchange(true, std::memory_order_acq_rel)) return;
  char id[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_music_first",
                      {{"kind", KindName(kind)},
                       {"music_event", sentinel::HexString(id, event)},
                       {"object", object},
                       {"in_combat_map", inCombatMap ? 1 : 0},
                       {"msg", "the first post of the combat music bank in this process: the module sees the game's music events"}});
}

// The handler. Everything that writes a log line runs after the game's function has returned.
std::uint64_t Post(tgt::Thunk::Fn original, void* self, std::uint32_t event, std::uint32_t object, std::uint32_t* flags, void* callback,
                   void* cookie, std::uint32_t seekMs, std::uint32_t* playingId) noexcept {
  const Kind kind = Classify(event);
  if (kind == Kind::kNone) return original(self, event, object, flags, callback, cookie, seekMs, playingId);  // every sound arena plays
  g_bankPosts.fetch_add(1, std::memory_order_relaxed);
  bool acted = false;
  bool inCombatMap = false;
  std::uint32_t previous = kNoObject;
  std::uint32_t stops = 0;
  if (kind == Kind::kPlay) {
    g_plays.fetch_add(1, std::memory_order_relaxed);
    inCombatMap = InCombatMapNow();
    if (inCombatMap) {
      acted = true;
      g_playsActed.fetch_add(1, std::memory_order_relaxed);
      previous = g_lastObject.exchange(object, std::memory_order_acq_rel);
      stops += PostBothStops(original, self, object);
      if (previous != object && previous != kNoObject) stops += PostBothStops(original, self, previous);
      g_self.store(reinterpret_cast<std::uintptr_t>(self), std::memory_order_relaxed);
      g_playing.store(true, std::memory_order_release);
    } else {
      g_playsOutOfScope.fetch_add(1, std::memory_order_relaxed);
    }
  }
  const std::uint64_t result = original(self, event, object, flags, callback, cookie, seekMs, playingId);
  SayFirst(kind, event, object, inCombatMap);
  if (kind == Kind::kPlay && g_playLines.fetch_add(1, std::memory_order_relaxed) < kMaxPlayLines) {
    char id[19];
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_music_play",
                        {{"music_event", sentinel::HexString(id, event)},
                         {"object", object},
                         {"prev_object", previous},
                         {"stops_posted", stops},
                         {"in_combat_map", acted ? 1 : 0},
                         {"n", g_plays.load(std::memory_order_relaxed)},
                         {"msg", acted ? "a combat music tree started: both music Stops were posted first, so only one instance plays"
                                       : "a combat music tree started with no combat map loaded: passed through untouched"}});
  }
  return result;
}

constexpr tgt::Thunk::Handler kHandler = &Post;
NEVR_HOOK_RECORD(kRecord, tgt::Thunk, kHandler);

// The tick callback. Publishes the game; from a Play until the combat map is gone it looks twice a second and stops the music once.
void OnTick(std::uintptr_t game) noexcept {
  if (g_game.load(std::memory_order_relaxed) != game) g_game.store(game, std::memory_order_release);
  if (!g_playing.load(std::memory_order_acquire)) return;  // arena, and a combat session before its first Play: one load
  const std::int64_t now = NowMs();
  if (now < g_nextLookMs.load(std::memory_order_relaxed)) return;
  g_nextLookMs.store(now + static_cast<std::int64_t>(kTickPeriodMs), std::memory_order_relaxed);
  std::uint64_t levelId = 0;
  if (level::CombatMapLoaded(game, &levelId)) return;
  if (!g_playing.exchange(false, std::memory_order_acq_rel)) return;
  const tgt::Thunk::Fn original = tgt::Thunk::Original();
  if (original == nullptr) return;
  const std::uint32_t object = g_lastObject.load(std::memory_order_acquire);
  void* const self = reinterpret_cast<void*>(g_self.load(std::memory_order_relaxed));
  const std::uint32_t stops = PostBothStops(original, self, object);
  g_tickStops.fetch_add(1, std::memory_order_relaxed);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_music_unload",
                      {{"object", object},
                       {"stops_posted", stops},
                       {"bank_posts", g_bankPosts.load(std::memory_order_relaxed)},
                       {"plays", g_plays.load(std::memory_order_relaxed)},
                       {"plays_out_of_scope", g_playsOutOfScope.load(std::memory_order_relaxed)},
                       {"msg", "the combat map is gone: both music Stops were posted, so the combat music does not play on in the lobby"}});
}

const char* FirstPinMismatch(const unsigned char* base) noexcept {
  for (const tgt::Word& pin : tgt::kWords) {
    std::uint32_t found = 0;
    std::memcpy(&found, base + pin.vaddr, sizeof(found));
    if (found != pin.word) return "post_sound_event_code";
  }
  return nullptr;
}

void LogInstall(sentinel::LogLevel level, const char* state, const char* reason, const char* status, const char* msg) noexcept {
  sentinel::LogFields(level, "combat_music_install",
                      {{"state", state}, {"reason", reason}, {"switch", "combat.music"}, {"scope", "combat_map"}, {"status", status}, {"msg", msg}});
}

void WarnLegacyKillFile() noexcept {
  if (!legacy::Present("music.off")) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_music_legacy_switch",
                      {{"file", "music.off"},
                       {"replaced_by", "combat.music: false"},
                       {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
}

constexpr const char* kRefusedMsg = "the combat music is not controlled: a second instance can play on top of the first";

}  // namespace

Kind Classify(std::uint32_t event) noexcept {
  std::size_t lo = 0, hi = kBank.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (kBank[mid].id == event) return kBank[mid].kind;
    if (kBank[mid].id < event) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return Kind::kNone;
}

ModuleResult InstallAt(bool configOn, sentinel::ImageLookup lookup, const targets::Pins& pins) noexcept {
  WarnLegacyKillFile();
  if (!configOn) {
    LogInstall(sentinel::LogLevel::kInfo, "off", "off_config", "none", "combat.music is false: the game's music is untouched");
    return {false, "off_config", true};  // off by its key: counted apart on the phase line, neither installed nor failed
  }
  const auto refuse = [](const char* reason, const char* status) noexcept -> ModuleResult {
    LogInstall(sentinel::LogLevel::kError, "refused", reason, status, kRefusedMsg);
    return {false, reason};
  };
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return refuse("module_not_loaded", "none");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return refuse("no_build_id", "none");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return refuse("build_id_mismatch", "none");
  if (FirstPinMismatch(reinterpret_cast<const unsigned char*>(image.base)) != nullptr) return refuse("pin_mismatch", "none");
  // The tick first: a module whose Stops at the end of the match could never run installs nothing.
  if (!g_tickRegistered.load(std::memory_order_acquire)) {
    if (!tick::Add(&Tick)) return refuse("tick_table_full", "none");
    g_tickRegistered.store(true, std::memory_order_release);
  }
  tgt::Thunk::Arm(kRecord);
  const sentinel::GotStatus status = sentinel::InstallThunk<tgt::Thunk>(g_hook, tgt::Target(image.base, pins), lookup);
  if (status != sentinel::GotStatus::kOk) {
    if (!g_hook.installed()) tgt::Thunk::Disarm();
    return refuse("hook", sentinel::GotStatusName(status));
  }
  g_on.store(true, std::memory_order_release);
  LogInstall(sentinel::LogLevel::kInfo, "on", "default", sentinel::GotStatusName(status),
             "PostSoundEvent posts both music Stops before each combat music Play and when the combat map unloads; an Echo Arena level and every other sound pass through");
  return {true, "installed"};
}

ModuleResult Install(bool configOn) noexcept { return InstallAt(configOn, &sentinel::FindLoadedImage, tgt::kPins); }

void Tick(std::uintptr_t game) noexcept { OnTick(game); }

Stats CurrentStats() noexcept {
  return {g_bankPosts.load(), g_plays.load(), g_playsActed.load(), g_playsOutOfScope.load(), g_stopsPosted.load(), g_tickStops.load()};
}

void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept { g_clock.store(clockMs, std::memory_order_release); }
void SetLegacyDirForTest(const char* dir) noexcept { legacy::SetDirectoryForTest(dir); }

void ResetForTest() noexcept {
  g_hook.Forget();
  tgt::Thunk::Reset();
  g_game.store(0);
  g_self.store(0);
  g_lastObject.store(kNoObject);
  g_playing.store(false);
  g_on.store(false);
  g_tickRegistered.store(false);
  g_firstSaid.store(false);
  g_playLines.store(0);
  g_bankPosts.store(0);
  g_plays.store(0);
  g_playsActed.store(0);
  g_playsOutOfScope.store(0);
  g_stopsPosted.store(0);
  g_tickStops.store(0);
  g_nextLookMs.store(0);
  g_clock.store(nullptr);
}

}  // namespace nevr_quest::combat::music

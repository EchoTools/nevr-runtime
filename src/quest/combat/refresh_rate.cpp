#include "quest/combat/refresh_rate.h"

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_active.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/headset_active.h"
#include "quest/headset_profile.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::refresh_rate {

namespace {

namespace headset_profile = nevr_quest::headset_profile;
namespace headset_active = nevr_quest::headset_active;

constexpr double kLookEverySeconds = 0.5;

// The rule, published once by the normal step; read by the handler (any thread) and the tick (the game thread).
std::atomic<bool> g_on{false};
std::atomic<bool> g_lobby{false};
std::atomic<bool> g_quest2{false};
std::atomic<float> g_levelHz{kLevelHz};

// What the game asked for and where: the tick needs both.
std::atomic<void*> g_ovr{nullptr};
std::atomic<float> g_session{0.0f};
std::atomic<bool> g_inLevel{false};
std::atomic<std::uint32_t> g_requests{0};
std::atomic<std::uint32_t> g_switches{0};
std::atomic<float> g_asked{-1.0f};
std::atomic<float> g_sent{-1.0f};
double g_nextLook = 0.0;  // the game thread only

// The thread making the tick's own re-ask (its kernel thread id), 0 when none: the handler and Note recognise it by thread.
std::atomic<long> g_reaskThread{0};

std::atomic<AskFn> g_ask{nullptr};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<double (*)() noexcept> g_clock{nullptr};

long ThisThread() noexcept { return static_cast<long>(syscall(SYS_gettid)); }
bool IsOwnReask() noexcept { return g_reaskThread.load(std::memory_order_acquire) == ThisThread(); }

double Now() noexcept {
  const auto clock = g_clock.load(std::memory_order_acquire);
  if (clock != nullptr) return clock();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// The headset is the identity step's profile (quest/headset_active.h, ADR 0031 decision 4): this file reads no system property. The step
// runs in the sentinel's constructor, long before this normal step; with no profile set, the generic profile and one Warning whose
// consequence is the one true in the mode the rule is in: on, it asks the generic profile's level rate; off (witness), it asks none and
// only the install line's `quest2` field takes the generic profile's value.
const headset_profile::Profile& Headset(bool ruleOn) noexcept {
  if (headset_active::Active() == nullptr) {
    if (ruleOn) {
      char hz[16];
      std::snprintf(hz, sizeof(hz), "%.0f", static_cast<double>(headset_profile::kGeneric.levelHz));
      sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_refresh_rate_profile_missing",
                          {{"profile", headset_profile::kGeneric.name},
                           {"level_hz", hz},
                           {"consequence", "the identity step set no headset profile before this step, so the rule asks the generic profile's level rate"}});
    } else {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_refresh_rate_profile_missing",
                          {{"profile", headset_profile::kGeneric.name},
                           {"consequence", "the identity step set no headset profile before this step; refresh_rate is off, so the rule asks no level rate and only the install line's quest2 field reads the generic profile's 0"}});
    }
  }
  return *headset_active::ActiveOrGeneric();
}

// The level rates are the headset profiles' (quest/headset_profile.h): a Quest 2 asks its profile's 72, every other profile the match rate.
static_assert(kQuest2LevelHz == headset_profile::kQuest2.levelHz, "the Quest 2 level rate is the Quest 2 profile's");
static_assert(kLevelHz == headset_profile::kQuest3.levelHz && kLevelHz == headset_profile::kGeneric.levelHz,
              "every other profile's level rate is the match rate");

// The plugin's kill files are not read: a file there turns nothing off (docs/adr/0046 C2). One found is a Warning that names the `combat:` key
// that replaces it, so an operator who relied on it learns why the rule is running.
void WarnLegacyKillFiles() noexcept {
  struct Legacy {
    const char* file;
    const char* key;
  };
  static constexpr Legacy kLegacy[] = {{"hzlevel.off", "combat.refresh_rate: false"},
                                       {"hzlobby90.off", "combat.refresh_rate_lobby: false (the default)"},
                                       {"hz72q2.off", "combat.refresh_rate_quest2_72: false"}};
  for (const Legacy& entry : kLegacy) {
    if (!legacy::Present(entry.file)) continue;
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_refresh_rate_legacy_switch",
                        {{"file", entry.file},
                         {"replaced_by", entry.key},
                         {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
  }
}

const char* ModeName() noexcept {
  if (!g_on.load(std::memory_order_relaxed)) return "witness";
  return g_lobby.load(std::memory_order_relaxed) ? "lobby" : "match";
}

Rule PublishedRule() noexcept {
  Rule rule;
  rule.on = g_on.load(std::memory_order_acquire);
  rule.lobby = g_lobby.load(std::memory_order_acquire);
  rule.levelHz = g_levelHz.load(std::memory_order_acquire);
  return rule;
}

// The function now in vrapi_SetDisplayRefreshRate's slot in libr15 (the render-configuration hook's thunk, or the game's own function
// when that hook is not installed), so the call goes through the manager's chain as the game's own call does.
int AskThroughTheSlot(void* ovr, float hz) noexcept {
  sentinel::ElfImage image{};
  sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  if (lookup == nullptr) lookup = &sentinel::FindLoadedImage;
  if (!lookup(sentinel::pinned::kLibR15, &image)) return -1;
  const sentinel::GotTarget target = sentinel::pinned::LibR15SetDisplayRefreshRate();
  if (!target.slotVaddr.has_value()) return -1;
  void** const slot = reinterpret_cast<void**>(image.base + *target.slotVaddr);
  void* const word = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (word == nullptr) return -1;
  return reinterpret_cast<sentinel::pinned::SetDisplayRefreshRateSig*>(word)(static_cast<sentinel::pinned::OvrMobileOpaque*>(ovr), hz);
}

int Ask(void* ovr, float hz) noexcept {
  AskFn fn = g_ask.load(std::memory_order_acquire);
  if (fn == nullptr) fn = &AskThroughTheSlot;
  g_reaskThread.store(ThisThread(), std::memory_order_release);
  const int result = fn(ovr, hz);
  g_reaskThread.store(0, std::memory_order_release);
  return result;
}

struct HzText {
  char text[24];
};
HzText Hz(float hz) noexcept {
  HzText out;
  std::snprintf(out.text, sizeof(out.text), "%.1f", static_cast<double>(hz));
  return out;
}

}  // namespace

Rule Decide(const Inputs& inputs) noexcept {
  Rule rule;
  rule.on = inputs.rule;
  rule.lobby = rule.on && inputs.lobby;
  rule.levelHz = rule.on && inputs.quest2 && inputs.quest2At72 ? kQuest2LevelHz : kLevelHz;
  return rule;
}

float RateFor(const Rule& rule, bool inLevel, float askedHz) noexcept {
  return rule.on && inLevel && askedHz > rule.levelHz ? rule.levelHz : askedHz;
}

float Choose(void* ovr, float askedHz) noexcept {
  if (!Active()) return askedHz;  // the release case: the game's own rate, nothing kept
  if (IsOwnReask()) return askedHz;  // the tick's re-ask is the answer already, not a request of the game's
  g_ovr.store(ovr, std::memory_order_release);
  g_session.store(askedHz, std::memory_order_release);
  return RateFor(PublishedRule(), g_inLevel.load(std::memory_order_acquire), askedHz);
}

void Note(float askedHz, float sentHz, int result) noexcept {
  if (!Active() || IsOwnReask()) return;
  const std::uint32_t n = g_requests.fetch_add(1, std::memory_order_relaxed) + 1;
  g_asked.store(askedHz, std::memory_order_relaxed);
  g_sent.store(sentHz, std::memory_order_relaxed);
  const HzText asked = Hz(askedHz);
  const HzText sent = Hz(sentHz);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_refresh_rate_request",
                      {{"n", n},
                       {"asked_hz", asked.text},
                       {"sent_hz", sent.text},
                       {"rc", result},
                       {"why", sentHz < askedHz ? "game request while a combat or lobby level is loaded: kept at the level rate" : "game request"},
                       {"mode", ModeName()},
                       {"quest2", g_quest2.load(std::memory_order_relaxed) ? 1 : 0}});
}

ModuleResult Install(bool rule, bool lobby, bool quest2At72) noexcept {
  if (!Active()) return {false, "inactive"};
  Inputs inputs;
  inputs.rule = rule;
  inputs.lobby = lobby;
  inputs.quest2At72 = quest2At72;
  inputs.quest2 = Headset(rule).id == headset_profile::Id::kQuest2;
  const Rule decided = Decide(inputs);
  g_quest2.store(inputs.quest2, std::memory_order_relaxed);
  g_levelHz.store(decided.levelHz, std::memory_order_release);
  g_lobby.store(decided.lobby, std::memory_order_release);
  g_on.store(decided.on, std::memory_order_release);  // last: the handler reads the rule whole once it is on
  const char* tickState = "not_needed";
  if (decided.on) tickState = tick::Add(&Tick) ? "registered" : "full";
  const bool full = std::strcmp(tickState, "full") == 0;
  char hz[16];
  std::snprintf(hz, sizeof(hz), "%.0f", static_cast<double>(decided.levelHz));
  sentinel::LogFields(full ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, "combat_refresh_rate_install",
                      {{"mode", ModeName()},
                       {"refresh_rate", inputs.rule ? 1 : 0},
                       {"refresh_rate_lobby", inputs.lobby ? 1 : 0},
                       {"refresh_rate_quest2_72", inputs.quest2At72 ? 1 : 0},
                       {"quest2", inputs.quest2 ? 1 : 0},
                       {"level_hz", hz},
                       {"tick", tickState},
                       {"msg", full ? "the game-thread tick table is full, so the rule only lowers requests the game makes; it never re-asks on a level change"
                           : !decided.on ? "refresh_rate is off: the game's own rate always, every request logged"
                                      : "combat maps (and, in lobby mode, the combat lobby) ask for the level rate; an Echo Arena level keeps the game's rate"}});
  WarnLegacyKillFiles();
  if (full) return {false, "tick_table_full"};
  return {true, decided.on ? "installed" : "witness"};
}

void Tick(std::uintptr_t game) noexcept {
  if (!g_on.load(std::memory_order_acquire)) return;
  const double now = Now();
  if (now < g_nextLook) return;
  g_nextLook = now + kLookEverySeconds;
  const float session = g_session.load(std::memory_order_acquire);
  const float low = g_levelHz.load(std::memory_order_acquire);
  void* const ovr = g_ovr.load(std::memory_order_acquire);
  if (session <= low || ovr == nullptr) return;  // no request seen yet, or nothing above the level rate to lower
  std::uint64_t level = 0;
  const level::Kind kind = level::Classify(game, g_lobby.load(std::memory_order_acquire), &level);
  const bool in = kind == level::Kind::kCombatOrLobby;
  if (in == g_inLevel.load(std::memory_order_acquire)) return;
  g_inLevel.store(in, std::memory_order_release);
  const float rate = in ? low : session;
  const int result = Ask(ovr, rate);
  g_sent.store(rate, std::memory_order_relaxed);
  const std::uint32_t n = g_switches.fetch_add(1, std::memory_order_relaxed) + 1;
  const HzText asked = Hz(session);
  const HzText sent = Hz(rate);
  char levelText[24];
  std::snprintf(levelText, sizeof(levelText), "%016llx", static_cast<unsigned long long>(level));
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_refresh_rate_switch",
                      {{"n", n},
                       {"asked_hz", asked.text},
                       {"sent_hz", sent.text},
                       {"rc", result},
                       {"level", levelText},
                       {"why", in ? "combat or lobby level loaded"
                               : kind == level::Kind::kEchoArena ? "echo arena level loaded: session rate"
                                                               : "no combat or lobby level: session rate"},
                       {"mode", ModeName()},
                       {"quest2", g_quest2.load(std::memory_order_relaxed) ? 1 : 0}});
}

Stats CurrentStats() noexcept {
  return {g_requests.load(), g_switches.load(), g_asked.load(), g_sent.load(), g_inLevel.load()};
}

void SetAskForTest(AskFn fn) noexcept { g_ask.store(fn, std::memory_order_release); }
void SetLegacyDirForTest(const char* dir) noexcept { legacy::SetDirectoryForTest(dir); }
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetClockForTest(double (*clock)() noexcept) noexcept { g_clock.store(clock, std::memory_order_release); }

void ResetForTest() noexcept {
  g_on.store(false);
  g_lobby.store(false);
  g_quest2.store(false);
  g_levelHz.store(kLevelHz);
  g_ovr.store(nullptr);
  g_session.store(0.0f);
  g_inLevel.store(false);
  g_requests.store(0);
  g_switches.store(0);
  g_asked.store(-1.0f);
  g_sent.store(-1.0f);
  g_nextLook = 0.0;
  g_reaskThread.store(0);
  g_lookup.store(nullptr);
}

}  // namespace nevr_quest::combat::refresh_rate

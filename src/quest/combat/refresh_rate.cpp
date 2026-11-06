#include "quest/combat/refresh_rate.h"

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "quest/combat/combat_active.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::refresh_rate {

namespace {

// mpl_combat_dyson / _combustion / _fission / _gauss: the level CSymbol64 a gamespace holds at +0x58.
constexpr std::uint64_t kCombatLevels[4] = {0x43e2da7914642604ULL, 0x42670f2bed45703cULL, 0xdf5ca7b7dfa383d4ULL, 0x43e2da7a0c623a19ULL};
// mpl_lobby_b / _b2 / _b2_lowspec / _b_combat / _b_arena / _b_social: the social lobby. Each is EchoVR::CSymbol64Hash of its name
// (sdk/include/nevr/symbol_hash.h), checked by refresh_rate_test.
constexpr std::uint64_t kLobbyLevels[6] = {0x5763ceff4e96bc48ULL, 0xd09afd15b1c75c04ULL, 0x05361c73bb73db19ULL,
                                           0xcb9977f7fc2b4526ULL, 0x6daa00a6d33d44b7ULL, 0xcb9967f7f220453eULL};
// mpl_arena_a / _a_lowspec / _b, mpl_tutorial_a: the Echo Arena game. Under the lobby rule an arena level keeps the session rate and wins
// over a lobby level still loaded during a transition.
constexpr std::uint64_t kEchoArenaLevels[4] = {0x576ed3f8428ebc4bULL, 0x7691d78d414c5701ULL, 0x576ed3f8428ebc48ULL, 0x3f20b99c97955721ULL};
// CR15Game's two gamespace tables {sym, gs*} (16-byte rows) and their counts, in the pinned build.
constexpr std::uintptr_t kTables[2][2] = {{0x1f48, 0x1f78}, {0x1f88, 0x1fb8}};
constexpr std::uint64_t kMaxGamespaces = 64;
constexpr std::uintptr_t kRowGamespace = 8;      // a table row's gamespace pointer
constexpr std::uintptr_t kGamespaceOwner = 0x8;  // a gamespace's CR15Game
constexpr std::uintptr_t kGamespaceLevel = 0x58;  // a gamespace's level CSymbol64
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
std::atomic<int> g_quest2Override{-1};
std::atomic<double (*)() noexcept> g_clock{nullptr};

long ThisThread() noexcept { return static_cast<long>(syscall(SYS_gettid)); }
bool IsOwnReask() noexcept { return g_reaskThread.load(std::memory_order_acquire) == ThisThread(); }

template <std::size_t N>
bool In(std::uint64_t level, const std::uint64_t (&set)[N]) noexcept {
  for (const std::uint64_t v : set) {
    if (level == v) return true;
  }
  return false;
}

double Now() noexcept {
  const auto clock = g_clock.load(std::memory_order_acquire);
  if (clock != nullptr) return clock();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// KNOWN-WRONG #2037: ADR 0031 decision 4 reserves the identity inputs to the identity step; this reads `ro.product.device` itself, as the
// q15t plugin did, until the headset profile that step chooses is handed to the combat modules. The Quest 2's codename is "hollywood".
bool ReadDeviceIsQuest2() noexcept {
  const int override = g_quest2Override.load(std::memory_order_relaxed);
  if (override >= 0) return override != 0;
#if defined(__ANDROID__)
  char value[PROP_VALUE_MAX] = {};
  __system_property_get("ro.product.device", value);
  return std::strcmp(value, "hollywood") == 0;
#else
  return false;
#endif
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
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return -1;
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

LevelKind ClassifyLoadedLevels(std::uintptr_t game, bool lobby, std::uint64_t* level) noexcept {
  std::uint64_t hit = 0;
  for (const auto& table : kTables) {
    bool ok = true;
    const auto rows = SafeRead<std::uintptr_t>(game + table[0], &ok);
    const auto count = SafeRead<std::uint64_t>(game + table[1], &ok);
    if (!ok || rows == 0 || count > kMaxGamespaces) continue;
    for (std::uint64_t i = 0; i < count; ++i) {
      bool rowOk = true;
      const auto gamespace = SafeRead<std::uintptr_t>(rows + i * 16 + kRowGamespace, &rowOk);
      if (!rowOk || gamespace == 0) continue;
      if (SafeRead<std::uintptr_t>(gamespace + kGamespaceOwner, &rowOk) != game || !rowOk) continue;
      const auto current = SafeRead<std::uint64_t>(gamespace + kGamespaceLevel, &rowOk);
      if (!rowOk) continue;
      if (lobby && In(current, kEchoArenaLevels)) {  // an Echo Arena level wins: the session rate
        *level = current;
        return LevelKind::kEchoArena;
      }
      if (hit == 0 && (In(current, kCombatLevels) || (lobby && In(current, kLobbyLevels)))) hit = current;
    }
  }
  *level = hit;
  return hit != 0 ? LevelKind::kCombatOrLobby : LevelKind::kNone;
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

ModuleResult InstallNormal() noexcept {
  if (!Active()) return {false, "inactive"};
  const nevr_quest::CombatConfig& config = Config();
  Inputs inputs;
  inputs.rule = config.refreshRate.on;
  inputs.lobby = config.refreshRateLobby.on;
  inputs.quest2At72 = config.refreshRateQuest2At72.on;
  inputs.quest2 = ReadDeviceIsQuest2();
  const Rule rule = Decide(inputs);
  g_quest2.store(inputs.quest2, std::memory_order_relaxed);
  g_levelHz.store(rule.levelHz, std::memory_order_release);
  g_lobby.store(rule.lobby, std::memory_order_release);
  g_on.store(rule.on, std::memory_order_release);  // last: the handler reads the rule whole once it is on
  const char* tickState = "not_needed";
  if (rule.on) tickState = tick::Add(&Tick) ? "registered" : "full";
  const bool full = std::strcmp(tickState, "full") == 0;
  char hz[16];
  std::snprintf(hz, sizeof(hz), "%.0f", static_cast<double>(rule.levelHz));
  sentinel::LogFields(full ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, "combat_refresh_rate_install",
                      {{"mode", ModeName()},
                       {"refresh_rate", inputs.rule ? 1 : 0},
                       {"refresh_rate_lobby", inputs.lobby ? 1 : 0},
                       {"refresh_rate_quest2_72", inputs.quest2At72 ? 1 : 0},
                       {"quest2", inputs.quest2 ? 1 : 0},
                       {"level_hz", hz},
                       {"tick", tickState},
                       {"msg", full ? "the game-thread tick table is full, so the rule only lowers requests the game makes; it never re-asks on a level change"
                           : !rule.on ? "refresh_rate is off: the game's own rate always, every request logged"
                                      : "combat maps (and, in lobby mode, the social lobby) ask for the level rate; an Echo Arena level keeps the game's rate"}});
  if (full) return {false, "tick_table_full"};
  return {true, rule.on ? "installed" : "witness"};
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
  const LevelKind kind = ClassifyLoadedLevels(game, g_lobby.load(std::memory_order_acquire), &level);
  const bool in = kind == LevelKind::kCombatOrLobby;
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
                               : kind == LevelKind::kEchoArena ? "echo arena level loaded: session rate"
                                                               : "no combat or lobby level: session rate"},
                       {"mode", ModeName()},
                       {"quest2", g_quest2.load(std::memory_order_relaxed) ? 1 : 0}});
}

Stats CurrentStats() noexcept {
  return {g_requests.load(), g_switches.load(), g_asked.load(), g_sent.load(), g_inLevel.load()};
}

void SetAskForTest(AskFn fn) noexcept { g_ask.store(fn, std::memory_order_release); }
void SetQuest2ForTest(int quest2) noexcept { g_quest2Override.store(quest2, std::memory_order_relaxed); }
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
}

}  // namespace nevr_quest::combat::refresh_rate

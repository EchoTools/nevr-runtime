#include "refresh_rate.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"

#ifndef Q15T_HZ_MATCH90
#define Q15T_HZ_MATCH90 0
#endif
#if !defined(PROP_VALUE_MAX)
#define PROP_VALUE_MAX 92
#endif

namespace q15t {
namespace {

using SetRefreshFn = std::int32_t (*)(void* ovr, float hz);

constexpr float kMatchHz = 90.f;
constexpr float kQuest2Hz = 72.f;  // q15t hz72q2: a Quest 2 ("hollywood") runs combat maps and the lobby at 72
// mpl_combat_dyson / _combustion / _fission / _gauss: the level CSymbol64 a gamespace holds at gs+0x58 (q15t the source build's hz)
constexpr std::uint64_t kCombatLevels[4] = {0x43e2da7914642604ull, 0x42670f2bed45703cull, 0xdf5ca7b7dfa383d4ull,
                                            0x43e2da7a0c623a19ull};
// CSymbol64 of mpl_lobby_b / _b2 / _b2_lowspec / _b_combat / _b_arena / _b_social: the social lobby (q15t hzlobby90). Each value is
// EchoVR::CSymbol64Hash of the name (sdk/include/nevr/symbol_hash.h), checked by the host test.
constexpr std::uint64_t kLobbyLevels[6] = {0x5763ceff4e96bc48ull, 0xd09afd15b1c75c04ull, 0x05361c73bb73db19ull,
                                           0xcb9977f7fc2b4526ull, 0x6daa00a6d33d44b7ull, 0xcb9967f7f220453eull};
// mpl_arena_a / _a_lowspec / _b, mpl_tutorial_a: the Echo Arena game. Under the lobby rule an arena level keeps the session rate and wins
// over a lobby level still loaded during a transition.
constexpr std::uint64_t kEchoArenaLevels[4] = {0x576ed3f8428ebc4bull, 0x7691d78d414c5701ull, 0x576ed3f8428ebc48ull,
                                               0x3f20b99c97955721ull};
// CR15Game's two gamespace tables {sym, gs*} (16-byte rows) and their counts (q15t the source build's gunconnect, build 4987566)
constexpr std::uintptr_t kTables[2][2] = {{0x1f48, 0x1f78}, {0x1f88, 0x1fb8}};
constexpr std::uint64_t kMaxGamespaces = 64;

std::atomic<std::uintptr_t> g_real{0};
std::atomic<void*> g_ovr{nullptr};
std::atomic<float> g_session{0.f};   // the game's own latest request
std::atomic<bool> g_in_match{false};
std::atomic<std::uint32_t> g_calls{0}, g_switches{0};
std::atomic<float> g_asked{-1.f}, g_sent{-1.f};
double g_next_look = 0;  // game thread only
// The level rule, read once at install (before any hook can run): on (Q15T_HZ_MATCH90 and no hzlevel.off), with the lobby (no
// hzlobby90.off), and the rate it asks for (90, or 72 on a Quest 2 unless hz72q2.off).
std::atomic<bool> g_rule{false}, g_lobby{false};
std::atomic<float> g_low{kMatchHz};
char g_device_override[96] = "";
bool g_have_device_override = false;

template <std::size_t N>
bool In(std::uint64_t level, const std::uint64_t (&set)[N]) {
  for (const std::uint64_t v : set)
    if (level == v) return true;
  return false;
}

void ReadDevice(char* out, std::size_t n) {
  out[0] = 0;
  if (g_have_device_override) {
    std::snprintf(out, n, "%s", g_device_override);
    return;
  }
#if defined(__ANDROID__)
  char v[PROP_VALUE_MAX] = "";
  __system_property_get("ro.product.device", v);
  std::snprintf(out, n, "%s", v);
#endif
}

const char* ModeName() {
  if (!g_rule.load(std::memory_order_relaxed)) return "witness";
  return g_lobby.load(std::memory_order_relaxed) ? "lobby" : "match";
}

double Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

std::int32_t CallReal(void* ovr, float hz) {
  const auto real = reinterpret_cast<SetRefreshFn>(g_real.load(std::memory_order_acquire));
  return real != nullptr ? real(ovr, hz) : -1;
}

void LogRate(const char* message, const char* why, std::uint32_t n, float asked, float sent, std::int32_t rc,
             std::uint64_t level) {
  char a[24], s[24], c[16], r[16], l[24];
  std::snprintf(a, sizeof a, "%.1f", static_cast<double>(asked));
  std::snprintf(s, sizeof s, "%.1f", static_cast<double>(sent));
  std::snprintf(c, sizeof c, "%u", n);
  std::snprintf(r, sizeof r, "%d", rc);
  std::snprintf(l, sizeof l, "%016llx", static_cast<unsigned long long>(level));
  const NevrLogField f[] = {{"n", c},  {"asked_hz", a}, {"sent_hz", s}, {"rc", r},
                            {"why", why}, {"level", l},  {"mode", ModeName()}};
  Log(NEVR_LOG_INFO, message, f, 7);
}

std::int32_t Wrapper(void* ovr, float hz) {
  const std::uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  g_ovr.store(ovr, std::memory_order_release);
  g_session.store(hz, std::memory_order_release);
  float send = hz;
  const char* why = "game request";
  const float low = g_low.load(std::memory_order_acquire);
  if (g_rule.load(std::memory_order_acquire) && hz > low && g_in_match.load(std::memory_order_acquire)) {
    send = low;
    why = "game request while a combat or lobby level is loaded: kept at the level rate";
  }
  const std::int32_t rc = CallReal(ovr, send);
  g_asked.store(hz, std::memory_order_relaxed);
  g_sent.store(send, std::memory_order_relaxed);
  LogRate("vrapi_SetDisplayRefreshRate", why, n, hz, send, rc, 0);  // the game calls this rarely: every call is logged
  return rc;
}

}  // namespace

bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level) {
  for (const auto& t : kTables) {
    bool ok = true;
    const auto tab = Rd<std::uintptr_t>(game + t[0], &ok);
    const auto cnt = Rd<std::uint64_t>(game + t[1], &ok);
    if (!ok || tab == 0 || cnt > kMaxGamespaces) continue;
    for (std::uint64_t i = 0; i < cnt; ++i) {
      bool ok2 = true;
      const auto gs = Rd<std::uintptr_t>(tab + i * 16 + 8, &ok2);
      if (!ok2 || gs == 0) continue;
      if (Rd<std::uintptr_t>(gs + 0x8, &ok2) != game || !ok2) continue;
      const auto lv = Rd<std::uint64_t>(gs + 0x58, &ok2);
      if (!ok2) continue;
      for (const std::uint64_t c : kCombatLevels) {
        if (lv == c) {
          *level = lv;
          return true;
        }
      }
    }
  }
  *level = 0;
  return false;
}

HzLevelKind ClassifyLoadedLevels(std::uintptr_t game, bool lobby, std::uint64_t* level) {
  std::uint64_t hit = 0;
  for (const auto& t : kTables) {
    bool ok = true;
    const auto tab = Rd<std::uintptr_t>(game + t[0], &ok);
    const auto cnt = Rd<std::uint64_t>(game + t[1], &ok);
    if (!ok || tab == 0 || cnt > kMaxGamespaces) continue;
    for (std::uint64_t i = 0; i < cnt; ++i) {
      bool ok2 = true;
      const auto gs = Rd<std::uintptr_t>(tab + i * 16 + 8, &ok2);
      if (!ok2 || gs == 0) continue;
      if (Rd<std::uintptr_t>(gs + 0x8, &ok2) != game || !ok2) continue;
      const auto lv = Rd<std::uint64_t>(gs + 0x58, &ok2);
      if (!ok2) continue;
      if (lobby && In(lv, kEchoArenaLevels)) {  // an Echo Arena level wins: the session rate
        *level = lv;
        return HzLevelKind::kEchoArena;
      }
      if (hit == 0 && (In(lv, kCombatLevels) || (lobby && In(lv, kLobbyLevels)))) hit = lv;
    }
  }
  *level = hit;
  return hit != 0 ? HzLevelKind::kCombatOrLobby : HzLevelKind::kNone;
}

void RefreshMatchTick(std::uintptr_t game) {
  if (!g_rule.load(std::memory_order_acquire)) return;
  const double now = Now();
  if (now < g_next_look) return;
  g_next_look = now + 0.5;
  const float session = g_session.load(std::memory_order_acquire);
  const float low = g_low.load(std::memory_order_acquire);
  void* ovr = g_ovr.load(std::memory_order_acquire);
  if (session <= low || ovr == nullptr) return;  // no request seen yet, or nothing above the level rate to lower
  std::uint64_t level = 0;
  const HzLevelKind kind = ClassifyLoadedLevels(game, g_lobby.load(std::memory_order_relaxed), &level);
  const bool in = kind == HzLevelKind::kCombatOrLobby;
  if (in == g_in_match.load(std::memory_order_relaxed)) return;
  g_in_match.store(in, std::memory_order_release);
  const float rate = in ? low : session;
  const std::int32_t rc = CallReal(ovr, rate);
  g_sent.store(rate, std::memory_order_relaxed);
  const std::uint32_t n = g_switches.fetch_add(1, std::memory_order_relaxed) + 1;
  const char* why = in ? "combat or lobby level loaded"
                       : kind == HzLevelKind::kEchoArena ? "echo arena level loaded: session rate"
                                                         : "no combat or lobby level: session rate";
  LogRate("refresh switch", why, n, session, rate, rc, level);
}

void ResetHzScheduleForTest() { g_next_look = 0; }

void SetHzDeviceForTest(const char* device) {
  g_have_device_override = device != nullptr;
  std::snprintf(g_device_override, sizeof g_device_override, "%s", device != nullptr ? device : "");
}

HzRule ConfigureHzRule() {
  HzRule r{};
  char device[96];
  ReadDevice(device, sizeof device);
  r.compiled = Q15T_HZ_MATCH90 != 0;
  r.level_off = ModuleSwitchPresent("hzlevel.off");
  r.lobby_off = ModuleSwitchPresent("hzlobby90.off");
  r.q2_off = ModuleSwitchPresent("hz72q2.off");
  r.quest2 = std::strcmp(device, "hollywood") == 0;
  r.on = r.compiled && !r.level_off;
  r.lobby = r.on && !r.lobby_off;
  r.low_hz = r.on && r.quest2 && !r.q2_off ? kQuest2Hz : kMatchHz;
  g_rule.store(r.on, std::memory_order_release);
  g_lobby.store(r.lobby, std::memory_order_release);
  g_low.store(r.low_hz, std::memory_order_release);
  char hz[16];
  std::snprintf(hz, sizeof hz, "%.0f", static_cast<double>(r.low_hz));
  const NevrLogField f[] = {{"mode", ModeName()},
                            {"compiled", r.compiled ? "1" : "0"},
                            {"hzlevel_off", r.level_off ? "1" : "0"},
                            {"hzlobby90_off", r.lobby_off ? "1" : "0"},
                            {"hz72q2_off", r.q2_off ? "1" : "0"},
                            {"device", device[0] != 0 ? device : "unknown"},
                            {"quest2", r.quest2 ? "1" : "0"},
                            {"level_hz", hz}};
  // one record per install: the rule the run uses (combat maps, and the lobby, at level_hz; an Echo Arena level keeps the game's rate)
  Log(NEVR_LOG_INFO, "q15t_refresh_rate_install", f, 8);
  return r;
}

HzStats GetHzStats() {
  return {g_calls.load(), g_switches.load(), g_asked.load(), g_sent.load(), g_in_match.load()};
}

InstallResult InstallRefreshRate(std::uintptr_t base, std::uintptr_t slot_vaddr) {
  ConfigureHzRule();  // before the swap: the wrapper reads the rule from its first call
  return InstallImportHook("vrapi_SetDisplayRefreshRate", base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&Wrapper),
                           &g_real);
}

}  // namespace q15t

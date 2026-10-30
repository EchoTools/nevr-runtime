#include "refresh_rate.h"

#include <atomic>
#include <cstdio>
#include <ctime>

#include "plugin_log.h"
#include "safe_read.h"

#ifndef Q15T_HZ_MATCH90
#define Q15T_HZ_MATCH90 0
#endif

namespace q15t {
namespace {

using SetRefreshFn = std::int32_t (*)(void* ovr, float hz);

constexpr float kMatchHz = 90.f;
// mpl_combat_dyson / _combustion / _fission / _gauss: the level CSymbol64 a gamespace holds at gs+0x58 (q15t the source build's hz)
constexpr std::uint64_t kCombatLevels[4] = {0x43e2da7914642604ull, 0x42670f2bed45703cull, 0xdf5ca7b7dfa383d4ull,
                                            0x43e2da7a0c623a19ull};
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
                            {"why", why}, {"level", l},  {"mode", Q15T_HZ_MATCH90 ? "match90" : "witness"}};
  Log(NEVR_LOG_INFO, message, f, 7);
}

std::int32_t Wrapper(void* ovr, float hz) {
  const std::uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  g_ovr.store(ovr, std::memory_order_release);
  g_session.store(hz, std::memory_order_release);
  float send = hz;
  const char* why = "game request";
#if Q15T_HZ_MATCH90
  if (hz > kMatchHz && g_in_match.load(std::memory_order_acquire)) {
    send = kMatchHz;
    why = "game request during a match: kept at 90";
  }
#endif
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

void RefreshMatchTick(std::uintptr_t game) {
  const double now = Now();
  if (now < g_next_look) return;
  g_next_look = now + 0.5;
  const float session = g_session.load(std::memory_order_acquire);
  void* ovr = g_ovr.load(std::memory_order_acquire);
  if (session <= kMatchHz || ovr == nullptr) return;  // no request seen yet, or nothing above 90 to lower
  std::uint64_t level = 0;
  const bool in = CombatMapLoaded(game, &level);
  if (in == g_in_match.load(std::memory_order_relaxed)) return;
  g_in_match.store(in, std::memory_order_release);
  const float rate = in ? kMatchHz : session;
  const std::int32_t rc = CallReal(ovr, rate);
  g_sent.store(rate, std::memory_order_relaxed);
  const std::uint32_t n = g_switches.fetch_add(1, std::memory_order_relaxed) + 1;
  LogRate("refresh switch", in ? "combat map loaded" : "combat map gone", n, session, rate, rc, level);
}

void ResetHzScheduleForTest() { g_next_look = 0; }

HzStats GetHzStats() {
  return {g_calls.load(), g_switches.load(), g_asked.load(), g_sent.load(), g_in_match.load()};
}

InstallResult InstallRefreshRate(std::uintptr_t base, std::uintptr_t slot_vaddr) {
  return InstallImportHook("vrapi_SetDisplayRefreshRate", base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&Wrapper),
                           &g_real);
}

}  // namespace q15t

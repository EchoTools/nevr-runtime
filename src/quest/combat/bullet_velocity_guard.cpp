// The bullet-velocity guard (bullet_velocity_guard.h, docs/quest/combat/bullet_velocity_guard.md): a thunk on FireBullet's slot that does not make a
// call whose velocity FireBullet cannot normalise.
#include "quest/combat/bullet_velocity_guard.h"

#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "core/burst_then_count.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "runtime/combat/bullet_velocity_guard_rules.h"

namespace nevr_quest::combat::bullet_velocity_guard {

namespace {

namespace rules = ::nevr_combat::bullet_velocity_guard;
using sentinel::LogLevel;

sentinel::GotHook g_hook;
std::atomic<std::uint64_t> g_calls{0}, g_skipped{0};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};
std::uint64_t (*g_clockMs)() = nullptr;
// The skip lines' budget (core/burst_then_count.h: no lock of its own). Taken only on the skip path, which calls nothing of the game's.
// A pthread mutex with a constant initializer, not a std::mutex: it has no destructor, so it adds no load-time initializer
// (tools/check_quest_static_init.sh), and its unlock carries no personality on the hook path (texture_menu_hook.cpp, g_keptMutex).
pthread_mutex_t g_budgetLock = PTHREAD_MUTEX_INITIALIZER;
nevr::BurstThenCount g_budget{kSkipLines, kSkipEveryMs};

constexpr const char* kSkipMsg = "a shot with no usable velocity was not fired: FireBullet would divide by zero and store a NaN direction";
constexpr const char* kInstalledMsg =
    "a FireBullet call whose velocity is null, at most about 1e-3 m/s or not finite is not made; every other call reaches FireBullet unchanged";
constexpr const char* kRefusedMsg = "the guard is not installed: a FireBullet call with no usable velocity still divides by zero";

std::uint64_t NowMs() noexcept {
  if (g_clockMs != nullptr) return g_clockMs();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

const char* F(char (&b)[32], float v) noexcept {
  std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
  return b;
}

// Written instead of the call to FireBullet, so no game frame is live above it.
[[gnu::noinline]] void LogSkip(const float* v, std::uint64_t handle, std::uint64_t n) noexcept {
  pthread_mutex_lock(&g_budgetLock);
  const nevr::BurstThenCount::Line l = g_budget.Event(NowMs());
  pthread_mutex_unlock(&g_budgetLock);
  const long long index = static_cast<long long>(handle & 0xffff);
  if (l.verdict == nevr::BurstThenCount::Verdict::kWrite) {
    if (v == nullptr) {
      sentinel::LogFields(LogLevel::kInfo, "combat_bullet_velocity_guard_skip", {{"n", n}, {"vel", "null"}, {"index", index}, {"msg", kSkipMsg}});
    } else {
      char x[32], y[32], z[32];
      sentinel::LogFields(LogLevel::kInfo, "combat_bullet_velocity_guard_skip",
                          {{"n", n}, {"vx", F(x, v[0])}, {"vy", F(y, v[1])}, {"vz", F(z, v[2])}, {"index", index}, {"msg", kSkipMsg}});
    }
  } else if (l.verdict == nevr::BurstThenCount::Verdict::kCapped) {
    sentinel::LogFields(LogLevel::kInfo, "combat_bullet_velocity_guard_skips",
                        {{"n", static_cast<long long>(l.total)}, {"since_last", 0},
                         {"msg", "later skipped shots are counted, not logged one by one; a count follows at most every 10 s while they continue"}});
  } else if (l.verdict == nevr::BurstThenCount::Verdict::kCount) {
    sentinel::LogFields(LogLevel::kInfo, "combat_bullet_velocity_guard_skips",
                        {{"n", static_cast<long long>(l.total)}, {"since_last", static_cast<long long>(l.sinceLine)},
                         {"msg", "shots with no usable velocity were not fired since the last line"}});
  }
}

std::uint64_t FireHandler(FireThunk::Fn original, std::uint8_t* bcs, std::uint64_t handle, const void* gun, float damage, const void* quat,
                          const void* vel, std::uint64_t remote, std::uint64_t flag) noexcept {
  g_calls.fetch_add(1, std::memory_order_relaxed);
  // FireBullet reads the three floats at x4 unconditionally, so this read is one the game makes anyway.
  const auto* v = static_cast<const float*>(vel);
  if (rules::Skips(v)) {
    LogSkip(v, handle, g_skipped.fetch_add(1, std::memory_order_relaxed) + 1);
    return 0;
  }
  return original(bcs, handle, gun, damage, quat, vel, remote, flag);
}

NEVR_HOOK_RECORD(kFireRecord, FireThunk, &FireHandler);

ModuleResult Refused(const char* status, std::uint64_t slot, const pinned::Failure* word = nullptr) noexcept {
  char s[19];
  if (word != nullptr) {
    char at[19], want[19], have[19];
    sentinel::LogFields(LogLevel::kError, "combat_bullet_velocity_guard_install",
                        {{"result", "refused"}, {"reason", status}, {"slot", sentinel::HexString(s, slot)}, {"pin", word->what},
                         {"vaddr", sentinel::HexString(at, word->vaddr)}, {"found", word->readable ? sentinel::HexString(have, word->have) : "unreadable"},
                         {"expected", sentinel::HexString(want, word->want)}, {"msg", kRefusedMsg}});
  } else {
    sentinel::LogFields(LogLevel::kError, "combat_bullet_velocity_guard_install",
                        {{"result", "refused"}, {"reason", status}, {"slot", sentinel::HexString(s, slot)}, {"msg", kRefusedMsg}});
  }
  return {false, status};
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins) noexcept {
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("module_not_loaded", pins.slot);
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id", pins.slot);
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", pins.slot);
  pinned::Failure word;
  if (pinned::FirstFailure(image.base, &targets::kWordPins, 1, &word)) return Refused("pinned_word_mismatch", pins.slot, &word);
  // dronemuzzle's target form (the slot must hold FireBullet itself, base + fn, or the chain's oldest original), owned by this module.
  sentinel::GotTarget target = dronemuzzle::Target(targets::kFireSymbol, pins.slot, image.base, pins.fn).Owned("combat_bullet_velocity_guard");
  FireThunk::Arm(kFireRecord);
  const sentinel::GotStatus status = sentinel::InstallThunk<FireThunk>(g_hook, target, lookup);
  if (status != sentinel::GotStatus::kOk) {
    if (!g_hook.installed()) FireThunk::Disarm();
    return Refused(sentinel::GotStatusName(status), pins.slot);
  }
  char s[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_bullet_velocity_guard_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"slot", sentinel::HexString(s, pins.slot)}, {"msg", kInstalledMsg}});
  return {true, "installed"};
}

ModuleResult Install(bool on) noexcept {
  if (!on) {
    char s[19];
    sentinel::LogFields(LogLevel::kInfo, "combat_bullet_velocity_guard_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"slot", sentinel::HexString(s, kPins.slot)},
                         {"msg", "combat.bullet_velocity_guard is false: a FireBullet call with no usable velocity still divides by zero"}});
    return {false, "off", true};
  }
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins);
}

void NoteEnd() noexcept {
  if (!g_hook.installed()) return;
  sentinel::LogFields(LogLevel::kInfo, "combat_bullet_velocity_guard_total",
                      {{"skipped", g_skipped.load(std::memory_order_relaxed)}, {"calls", g_calls.load(std::memory_order_relaxed)},
                       {"msg", "calls is every FireBullet call that reached the guard, skipped those not made for want of a usable velocity"}});
}

std::uint64_t Skipped() noexcept { return g_skipped.load(std::memory_order_relaxed); }
std::uint64_t Calls() noexcept { return g_calls.load(std::memory_order_relaxed); }

void SetInstallForTest(sentinel::ImageLookup lookup, const Pins* pins) noexcept {
  g_lookup.store(lookup, std::memory_order_release);
  g_pinsOverride.store(pins, std::memory_order_release);
}
void SetClockForTest(std::uint64_t (*nowMs)()) noexcept { g_clockMs = nowMs; }

void ResetForTest() noexcept {
  g_hook.Forget();
  FireThunk::Reset();
  g_calls.store(0);
  g_skipped.store(0);
  g_budget = nevr::BurstThenCount{kSkipLines, kSkipEveryMs};
  g_clockMs = nullptr;
  SetInstallForTest(nullptr, nullptr);
}

}  // namespace nevr_quest::combat::bullet_velocity_guard

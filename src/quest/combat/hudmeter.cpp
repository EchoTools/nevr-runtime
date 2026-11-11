#include "quest/combat/hudmeter.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_scope.h"
#include "quest/combat/hudmeter_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::hudmeter {

namespace {

constexpr unsigned kFillLines = 8;
constexpr float kFull[2] = {kFullX, kFullY};

sentinel::GotHook g_hook;
std::atomic<unsigned> g_full{0}, g_guarded{0}, g_unreadable{0}, g_fillLines{0};
std::atomic<bool> g_passed{false};
std::atomic<std::uint32_t> g_lastY{0};
std::atomic<std::uint64_t> g_otherComp{0};

template <typename T>
T At(const std::uint8_t* p, std::uintptr_t off) noexcept {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

const char* Num(char (&buf)[24], double v) noexcept {
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

__attribute__((noinline, cold)) void LogFull(unsigned n, std::uint64_t comp, std::uint64_t item) noexcept {
  if (n & (n - 1)) return;  // 1st, 2nd, 4th, 8th ...
  char x[24], y[24], c[19], i[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_hudmeter_full",
                      {{"n", n}, {"from_x", "0"}, {"from_y", "0"}, {"to_x", Num(x, kFullX)}, {"to_y", Num(y, kFullY)},
                       {"comp", sentinel::HexString(c, comp)}, {"item", sentinel::HexString(i, item)},
                       {"msg", "a 0 x 0 weapon_meter write was made the authored full size so the heat bar can fill"}});
}

__attribute__((noinline, cold)) void LogFill(float x, float y) noexcept {  // the first kFillLines size writes that change the height
  std::uint32_t yb;
  std::memcpy(&yb, &y, sizeof yb);
  if (g_lastY.exchange(yb, std::memory_order_relaxed) == yb) return;
  if (g_fillLines.fetch_add(1, std::memory_order_relaxed) >= kFillLines) return;
  char xs[24], ys[24];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_hudmeter_fill",
                      {{"x", Num(xs, x)}, {"y", Num(ys, y)}, {"msg", "the weapon_meter size changed; the heat fill is live"}});
}

__attribute__((noinline, cold)) void LogOtherComp(std::uint64_t comp) noexcept {  // the first component other than the lobby helmet's, once
  std::uint64_t none = 0;
  if (!g_otherComp.compare_exchange_strong(none, comp, std::memory_order_relaxed)) return;
  char c[19], l[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_hudmeter_component",
                      {{"comp", sentinel::HexString(c, comp)}, {"lobby_helmet_comp", sentinel::HexString(l, kLobbyComponent)},
                       {"msg", "weapon_meter on another canvas component; guarded as well"}});
}

__attribute__((noinline, cold)) void LogPass(float x, float y) noexcept {  // the first other size after a run of full writes: the fill is live
  if (g_passed.exchange(true, std::memory_order_relaxed)) return;
  char xs[24], ys[24];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_hudmeter_pass",
                      {{"x", Num(xs, x)}, {"y", Num(ys, y)}, {"full_writes_before", g_full.load(std::memory_order_relaxed)},
                       {"msg", "the full size was read, so the heat fill is live"}});
}

// The call in a combat level. Not inlined into the handler: its stack buffer and its log calls would give the handler a frame, a stack canary and
// saved registers on the path of an arena match, which must be a load, a test and a tail call.
[[gnu::noinline]] std::uint64_t ActInCombat(Thunk::Fn original, void* node, const void* data) noexcept {
  if (!scope::LookIsFresh()) return original(node, data);  // the tick stopped: the answer is not believed (combat_scope.h)
  std::uint8_t d[kDataPrefix];
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(data), d, sizeof d)) {  // on any doubt the real function, untouched
    g_unreadable.fetch_add(1, std::memory_order_relaxed);
    return original(node, data);
  }
  if (At<std::uint64_t>(d, kDataItem) != kItem || At<std::int32_t>(d, kDataSizeOn) == 0) return original(node, data);
  g_guarded.fetch_add(1, std::memory_order_relaxed);
  // any canvas component (rev 3: the lobby helmet's is not the only one a match uses); the first other one is named once
  if (At<std::uint64_t>(d, kDataComponent) != kLobbyComponent) LogOtherComp(At<std::uint64_t>(d, kDataComponent));
  const float x = At<float>(d, kDataSize), y = At<float>(d, kDataSize + 4);
  if (x != 0.0f || y != 0.0f) {
    if (!g_passed.load(std::memory_order_relaxed)) {
      LogPass(x, y);
    } else {
      LogFill(x, y);
    }
    return original(node, data);
  }
  LogFull(g_full.fetch_add(1, std::memory_order_relaxed) + 1, At<std::uint64_t>(d, kDataComponent), At<std::uint64_t>(d, kDataItem));
  g_passed.store(false, std::memory_order_relaxed);  // one pass line per helmet start
  // This call only: Enter sets the authored size, then the node's own value is put back. The bytes were just read, so the range is mapped;
  // the buffer is the calling script thread's own node-data buffer (thread + 0xb0).
  auto* size = reinterpret_cast<std::uint8_t*>(const_cast<void*>(data)) + kDataSize;
  std::memcpy(size, kFull, sizeof kFull);
  const std::uint64_t result = original(node, data);
  std::memcpy(size, d + kDataSize, sizeof kFull);  // what was there: 0 x 0
  return result;
}

std::uint64_t OnEnter(Thunk::Fn original, void* node, const void* data) noexcept {
  // Outside a combat level (an Echo Arena level, a lobby arena players use, anywhere else) the game's function with the same arguments, and
  // nothing else: no read of the data, no write, no counter.
  if (!scope::SeenCombatOrLobby()) return original(node, data);
  return ActInCombat(original, node, data);
}

NEVR_HOOK_RECORD(kHudmeterHook, Thunk, &OnEnter);

ModuleResult Refused(const char* token, const char* consequence) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_hudmeter_install",
                      {{"result", "refused"}, {"reason", token}, {"msg", consequence}});
  return {false, token};
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, bool on) noexcept {
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_hudmeter_install",
                        {{"result", "off"}, {"reason", "off_config"},
                         {"msg", "combat.hudmeter is false: the wrist HUD's heat fill is the game's own (0 x 0 on the Quest)"}});
    return {false, "off", true};
  }
  if (!scope::Live()) return Refused("scope_not_live", "the combat level answer cannot change (the tick or its refresh is not installed), so the module is not installed");
  Thunk::Arm(kHudmeterHook);
  sentinel::ElfImage image{};
  sentinel::GotStatus status = lookup != nullptr && lookup(sentinel::pinned::kLibR15, &image) ? sentinel::GotStatus::kOk
                                                                                              : sentinel::GotStatus::kModuleNotLoaded;
  if (status == sentinel::GotStatus::kOk) status = sentinel::InstallThunk<Thunk>(g_hook, Target(image.base), lookup);
  const bool ok = status == sentinel::GotStatus::kOk;
  if (!ok) Thunk::Disarm();
  char full[2][24];
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_hudmeter_install",
                      {{"result", ok ? "installed" : "refused"}, {"reason", ok ? "ok" : sentinel::GotStatusName(status)},
                       {"full_x", Num(full[0], kFullX)}, {"full_y", Num(full[1], kFullY)},
                       {"msg", ok ? "a 0 x 0 size write to weapon_meter becomes the authored full size, in a combat level only"
                                  : "the wrist HUD's heat fill is the game's own (0 x 0 on the Quest)"}});
  return {ok, ok ? "installed" : sentinel::GotStatusName(status)};
}

ModuleResult Install(bool on) noexcept { return InstallAt(&sentinel::FindLoadedImage, on); }

Stats CurrentStats() noexcept { return {g_guarded.load(), g_full.load(), g_unreadable.load()}; }

void ResetForTest() noexcept {
  g_hook.Forget();
  Thunk::Reset();
  g_full = g_guarded = g_unreadable = g_fillLines = 0;
  g_passed = false;
  g_lastY = 0;
  g_otherComp = 0;
}

}  // namespace nevr_quest::combat::hudmeter

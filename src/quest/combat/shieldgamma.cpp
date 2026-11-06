#include "quest/combat/shieldgamma.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_scope.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/shieldgamma_targets.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::shieldgamma {

namespace {

sentinel::GotHook g_hook;
std::atomic<unsigned> g_decoded{0}, g_mirrored{0}, g_unreadable{0};
std::atomic<bool> g_gamma{false}, g_mirror{false};

template <typename T>
T At(const std::uint8_t* p, std::uintptr_t off) noexcept {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

float Decode(float x) noexcept { return std::pow(x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x), kGamma); }

__attribute__((noinline, cold)) void LogDecode(const char* event, unsigned n, std::uint64_t prop, const float* from, const float* to) noexcept {
  if (n & (n - 1)) return;  // 1st, 2nd, 4th, 8th ...
  char p[19], v[6][24];
  for (int i = 0; i < 3; ++i) {
    std::snprintf(v[i], sizeof v[i], "%g", static_cast<double>(from[i]));
    std::snprintf(v[3 + i], sizeof v[3 + i], "%g", static_cast<double>(to[i]));
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, event,
                      {{"n", n}, {"prop", sentinel::HexString(p, prop)}, {"r", v[0]}, {"g", v[1]}, {"b", v[2]}, {"to_r", v[3]},
                       {"to_g", v[4]}, {"to_b", v[5]},
                       {"msg", "a team colour written to the shield material was decoded as the PC's pixel shader does"}});
}

// The call in a combat level; returns the game's own result. Not inlined into the handler: its stack buffers and its log calls would give the
// handler a frame, a stack canary and saved registers on the path of an arena match, which must be a load, a test and a tail call.
[[gnu::noinline]] std::uint64_t ActInCombat(Thunk::Fn original, void* node, const void* data) noexcept {
  if (!scope::LookIsFresh()) return original(node, data);  // the tick stopped: the answer is not believed (combat_scope.h)
  std::uint8_t d[kDataSize];
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(data), d, sizeof d)) {  // on any doubt the real function, untouched
    g_unreadable.fetch_add(1, std::memory_order_relaxed);
    return original(node, data);
  }
  if (At<std::uint64_t>(d, kDataMaterial) != kMaterial || At<std::int32_t>(d, kDataType) != kVec4 || At<std::int32_t>(d, kDataSpace) != kLinear)
    return original(node, data);
  const std::uint64_t prop = At<std::uint64_t>(d, kDataProperty);
  float was[3], now[3];
  std::memcpy(was, d + kDataValue, sizeof was);
  for (int i = 0; i < 3; ++i) now[i] = Decode(was[i]);
  if (g_mirror.load(std::memory_order_relaxed) && prop == kRingProperty) {
    // Update only resolves the actor's model, calls CModelCS::SetMaterialProperty and reads the node's start time, so a second call on a copy
    // with the material set to the tube window's is the write alone. The real call's result is kept.
    std::uint8_t copy[kDataSize];
    std::memcpy(copy, d, sizeof copy);
    std::memcpy(copy + kDataMaterial, &kTubeMaterial, sizeof kTubeMaterial);
    std::memcpy(copy + kDataValue, now, sizeof now);
    LogDecode("combat_shieldgamma_mirror", g_mirrored.fetch_add(1, std::memory_order_relaxed) + 1, prop, was, now);
    original(node, copy);
  }
  if (!g_gamma.load(std::memory_order_relaxed)) return original(node, data);
  LogDecode("combat_shieldgamma_decode", g_decoded.fetch_add(1, std::memory_order_relaxed) + 1, prop, was, now);
  // This call only: Update hands the decoded rgb to SetMaterialProperty (it loads s0, s1 from +0x38); the node's own value is put back after it.
  // The bytes were just read, so the range is mapped; the buffer is the script thread's own.
  auto* value = reinterpret_cast<std::uint8_t*>(const_cast<void*>(data)) + kDataValue;
  std::memcpy(value, now, sizeof now);
  const std::uint64_t result = original(node, data);
  std::memcpy(value, was, sizeof was);
  return result;
}

// Per update while the duration runs.
std::uint64_t OnUpdate(Thunk::Fn original, void* node, const void* data) noexcept {
  // Outside a combat level (an Echo Arena level, a lobby arena players use, anywhere else) the game's function with the same arguments, and
  // nothing else: no read of the data, no write, no counter, no second call.
  if (!scope::SeenCombatOrLobby()) return original(node, data);
  return ActInCombat(original, node, data);
}

NEVR_HOOK_RECORD(kShieldGammaHook, Thunk, &OnUpdate);

ModuleResult Refused(const char* token, const char* consequence, bool gamma, bool tubeMirror) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_shieldgamma_install",
                      {{"result", "refused"}, {"reason", token}, {"gamma", gamma ? 1 : 0}, {"tube_mirror", tubeMirror ? 1 : 0}, {"msg", consequence}});
  return {false, token};
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, bool gamma, bool tubeMirror) noexcept {
  if (!gamma && !tubeMirror) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_shieldgamma_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"gamma", 0}, {"tube_mirror", 0},
                         {"msg", "combat.shieldgamma_gamma and combat.shieldgamma_tube_mirror are both false: nothing is installed and the game's colours stand"}});
    return {false, "off"};
  }
  if (!scope::Live()) return Refused("scope_not_live", "the combat level answer cannot change (the tick or its refresh is not installed), so the module is not installed", gamma, tubeMirror);
  g_gamma.store(gamma, std::memory_order_relaxed);  // before the thunk is installed, never changed after it
  g_mirror.store(tubeMirror, std::memory_order_relaxed);
  Thunk::Arm(kShieldGammaHook);
  sentinel::ElfImage image{};
  sentinel::GotStatus status = lookup != nullptr && lookup(sentinel::pinned::kLibR15, &image) ? sentinel::GotStatus::kOk
                                                                                              : sentinel::GotStatus::kModuleNotLoaded;
  if (status == sentinel::GotStatus::kOk) status = sentinel::InstallThunk<Thunk>(g_hook, Target(image.base), lookup);
  const bool ok = status == sentinel::GotStatus::kOk;
  if (!ok) Thunk::Disarm();
  char material[19];
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_shieldgamma_install",
                      {{"result", ok ? "installed" : "refused"}, {"reason", ok ? "ok" : sentinel::GotStatusName(status)},
                       {"gamma", gamma ? 1 : 0}, {"tube_mirror", tubeMirror ? 1 : 0}, {"material", sentinel::HexString(material, kMaterial)},
                       {"msg", ok ? "the shield material's team colour is decoded and the tube windows follow it, in a combat level only"
                                  : "the spawn-tube windows and the barrier panes keep the game's colours"}});
  return {ok, ok ? "installed" : sentinel::GotStatusName(status)};
}

ModuleResult Install(bool gamma, bool tubeMirror) noexcept { return InstallAt(&sentinel::FindLoadedImage, gamma, tubeMirror); }

Stats CurrentStats() noexcept { return {g_decoded.load(), g_mirrored.load(), g_unreadable.load()}; }

void ResetForTest() noexcept {
  g_hook.Forget();
  Thunk::Reset();
  g_decoded = g_mirrored = g_unreadable = 0;
  g_gamma = g_mirror = false;
}

}  // namespace nevr_quest::combat::shieldgamma

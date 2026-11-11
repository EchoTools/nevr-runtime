#include "quest/combat/fxtrail.h"

#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_scope.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::fxtrail {

namespace {

sentinel::GotHook g_hook;
std::atomic<std::uint64_t> g_inCombat{0};
std::atomic<std::uint64_t> g_ended{0};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};

constexpr const char* kConsequence = "a bullet whose trail effect has no instance ends its trail, so it can be freed, while a combat map is loaded";
constexpr const char* kRefusedConsequence = "bullets whose trail effect has no instance are never freed; the game's deferred-call queue overflows in a long combat match";

void LogStats(bool final) noexcept {
  sentinel::LogFields(final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, "combat_fxtrail_stats",
                      {{"calls", EffectTimeThunk::Calls()},
                       {"in_combat", g_inCombat.load(std::memory_order_relaxed)},
                       {"ended", g_ended.load(std::memory_order_relaxed)},
                       {"final", final ? 1 : 0},
                       {"consequence", "calls is every EffectTime call the game made, in_combat those made while a combat map was loaded (the only ones "
                                       "read), ended the trails ended"}});
}

// Written after the game's function returned, so no game frame is live above it.
void LogEnded(std::uint64_t n) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_fxtrail_ended",
                      {{"n", n}, {"consequence", "an effect with no instance answered FLT_MAX, so its bullet's trail slot is finished and the bullet can be freed"}});
}

// The call in a combat map. Not inlined into the handler: its reads and its log calls would give the handler a frame, a stack canary and saved registers
// on the path of an arena match, which must be a load, a test and a tail call. The game's value, except FLT_MAX for an effect with no instance. Every
// read can fail: then the game's value stands.
[[gnu::noinline]] float ActInCombatMap(EffectTimeThunk::Fn original, const void* cs, std::uint64_t handle) noexcept {
  const float time = original(cs, handle);
  if (!scope::LookIsFresh()) return time;  // the tick stopped: the answer is not believed (combat_scope.h)
  const std::uint64_t inCombat = g_inCombat.fetch_add(1, std::memory_order_relaxed) + 1;
  bool ok = true;
  const auto base = reinterpret_cast<std::uintptr_t>(cs);
  const auto index = SafeRead<std::uintptr_t>(base + kOffIndex, &ok);
  const auto data = SafeRead<std::uintptr_t>(base + kOffData, &ok);
  const auto slot = SafeRead<std::uint16_t>(index + (handle & 0xffff) * 4, &ok);  // a failed read leaves ok false for the rest
  const auto instance = SafeRead<std::uintptr_t>(data + static_cast<std::uintptr_t>(slot) * kDataStride + kOffInstance, &ok);
  if ((inCombat & (kStatsEvery - 1)) == 0) LogStats(false);
  if (!ok || instance != 0) return time;
  const std::uint64_t n = g_ended.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((n & (n - 1)) == 0) LogEnded(n);  // n = 1, 2, 4, 8, ...
  return FLT_MAX;
}

float EffectTimeHandler(EffectTimeThunk::Fn original, const void* cs, std::uint64_t handle) noexcept {
  // Outside a combat map (an Echo Arena level, every lobby, any other level) the game's function with the same arguments and nothing else: no read of the
  // game's memory, no counter, no line.
  if (!scope::SeenCombatMap()) return original(cs, handle);
  return ActInCombatMap(original, cs, handle);
}

NEVR_HOOK_RECORD(kEffectTimeRecord, EffectTimeThunk, &EffectTimeHandler);

ModuleResult Refused(const char* token, const pinned::Failure* word = nullptr) noexcept {
  if (word != nullptr) {
    char at[19], haveText[19], wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_fxtrail_install",
                        {{"result", "refused"}, {"reason", token}, {"pin", word->what}, {"vaddr", sentinel::HexString(at, word->vaddr)},
                         {"found", word->readable ? sentinel::HexString(haveText, word->have) : "unreadable"},
                         {"expected", sentinel::HexString(wantText, word->want)}, {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_fxtrail_install",
                        {{"result", "refused"}, {"reason", token}, {"consequence", kRefusedConsequence}});
  }
  return {false, token};
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept {
  // The old plugin's switch file changes nothing now; one that is there is told so.
  if (legacy::Present("fxtrail.off")) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_fxtrail_legacy_switch",
                        {{"key", "combat.fxtrail"}, {"file", "fxtrail.off"},
                         {"consequence", "the file is no longer read; set combat.fxtrail: false in config.yaml to turn the module off"}});
  }
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_fxtrail_install",
                        {{"result", "off"}, {"reason", "off_config"},
                         {"consequence", "combat.fxtrail is false: the game's own EffectTime answers, and bullets with a trail are never freed"}});
    return {false, "off_config", true};
  }
  // The guard acts only while a combat map is loaded, which the scope's answer says: when that answer cannot change (the tick or the scope row's refresh is
  // not installed) the hook would never act, so it is not installed (combat_scope.h `Live`).
  if (!scope::Live()) return Refused("scope_not_live");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch");
  pinned::Failure word;
  if (pinned::FirstFailure(image.base, &kWordPins, 1, &word)) return Refused("pinned_word_mismatch", &word);
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kEffectTimeSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), pins.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(image.base + pins.fn);
  target = target.Owned("combat/fxtrail");
  EffectTimeThunk::Arm(kEffectTimeRecord);
  const sentinel::GotStatus status = sentinel::InstallThunk<EffectTimeThunk>(g_hook, target, lookup);
  const bool installed = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_fxtrail_install",
                      {{"result", installed ? "installed" : "refused"}, {"reason", installed ? "ok" : "slot_refused"},
                       {"hook", sentinel::GotStatusName(status)}, {"scope", "combat_map"},
                       {"consequence", installed ? kConsequence : kRefusedConsequence}});
  return {installed, installed ? "installed" : sentinel::GotStatusName(status)};
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, kPins, on);
}

void NoteEnd() noexcept {
  if (g_hook.installed()) LogStats(true);
}

Stats CurrentStats() noexcept {
  return {EffectTimeThunk::Calls(), g_inCombat.load(std::memory_order_relaxed), g_ended.load(std::memory_order_relaxed)};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  EffectTimeThunk::Reset();
  g_inCombat.store(0);
  g_ended.store(0);
  g_lookup.store(nullptr);
}

}  // namespace nevr_quest::combat::fxtrail

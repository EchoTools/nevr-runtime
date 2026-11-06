#include "quest/combat/fxtrail.h"

#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>

#include "quest/combat/legacy_switch.h"
#include "quest/combat/level_scope.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::fxtrail {

namespace {

namespace pinned = sentinel::pinned;

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
                       {"level_looks", scope::Looks()},
                       {"final", final ? 1 : 0},
                       {"consequence", "calls is every EffectTime call the game made, in_combat those made while a combat map was loaded (the only ones "
                                       "read), ended the trails ended; level_looks is 0 when the level scope never ran, and then nothing is ended"}});
}

// Written after the game's function returned, so no game frame is live above it.
void LogEnded(std::uint64_t n) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_fxtrail_ended",
                      {{"n", n}, {"consequence", "an effect with no instance answered FLT_MAX, so its bullet's trail slot is finished and the bullet can be freed"}});
}

// The game's value, except FLT_MAX for an effect with no instance while a combat map is loaded. Every read can fail: then the game's value stands.
float EffectTimeHandler(EffectTimeThunk::Fn original, const void* cs, std::uint64_t handle) noexcept {
  const float time = original(cs, handle);
  if (!scope::InCombatMap()) return time;  // arena, every lobby, every other level: the game's value, nothing read
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

NEVR_HOOK_RECORD(kEffectTimeRecord, EffectTimeThunk, &EffectTimeHandler);

ModuleResult Refused(const char* token, const char* pin = nullptr, const char* found = nullptr, const char* expected = nullptr) noexcept {
  if (pin != nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_fxtrail_install",
                        {{"result", "refused"}, {"reason", token}, {"pin", pin}, {"found", found}, {"expected", expected},
                         {"consequence", kRefusedConsequence}});
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
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refused("module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id");
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch");
  std::uint32_t have = 0;
  bool readable = false;
  const std::size_t bad = FirstWordMismatch(image.base, kWords, kWordCount, &have, &readable);
  if (bad != kWordCount) {
    char at[19];
    char haveText[19];
    char wantText[19];
    return Refused("pinned_word_mismatch", sentinel::HexString(at, kWords[bad].vaddr),
                   readable ? sentinel::HexString(haveText, have) : "unreadable", sentinel::HexString(wantText, kWords[bad].word));
  }
  if (!scope::Start()) return Refused("tick_table_full");  // the guard would never be in a combat map, so it is not installed
  sentinel::GotTarget target(pinned::kLibR15, kEffectTimeSymbol, sentinel::RelocKind::kJumpSlot, pinned::LibR15PatchedBuildId(), pins.slot);
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

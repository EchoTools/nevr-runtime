#include "quest/combat/tablestep.h"

#include <atomic>
#include <cstring>

#include "quest/combat/table_step.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::tablestep {

namespace {

sentinel::GotHook g_hook;
std::atomic<bool> g_ran{false};
std::atomic<std::uint32_t> g_rewritten{0};
std::atomic<std::uint32_t> g_unexpected{0};
std::atomic<std::uint32_t> g_stock{0};
std::atomic<bool> g_saidFirst{false};
std::atomic<bool> g_saidUnexpected{false};
std::atomic<bool> g_saidStock{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};

table_step::Counts CountsOf() noexcept {
  return {g_stock.load(std::memory_order_relaxed) + g_unexpected.load(std::memory_order_relaxed), g_rewritten.load(std::memory_order_relaxed)};
}

void SayStock(table_step::Scope scope) noexcept {
  if (g_saidStock.exchange(true, std::memory_order_relaxed)) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_tablestep_stock",
                      {{"scope", table_step::ScopeName(scope)},
                       {"msg", "the first shader set created outside a combat match is the game's own, growth step 32"}});
}

// The game's function runs first, for every call; in a combat match the step of the object it returned is then corrected. The log is written
// after the work, never while a game call is on the stack above it (callback_thunk.h rule 1).
void* Handler(Thunk::Fn original, void* unused, void* init) noexcept {
  const table_step::Scope scope = table_step::CurrentScope();
  void* const obj = original(unused, init);
  if (obj == nullptr) return obj;  // the game's own result, unchanged
  if (scope != table_step::Scope::kCombat) {
    g_stock.fetch_add(1, std::memory_order_relaxed);
    SayStock(scope);
    return obj;
  }
  std::uint64_t found = 0;
  switch (table_step::ApplyShaderSetStep(obj, &found)) {
    case table_step::StepFix::kSet:
      g_rewritten.fetch_add(1, std::memory_order_relaxed);
      if (!g_saidFirst.exchange(true, std::memory_order_relaxed)) {
        sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_tablestep_first",
                            {{"from", table_step::kShaderSetStockStep}, {"to", table_step::kShaderSetStep},
                             {"msg", "the first shader set created in a combat match has the table growth step 1"}});
      }
      break;
    case table_step::StepFix::kNotStock:
      g_unexpected.fetch_add(1, std::memory_order_relaxed);
      if (!g_saidUnexpected.exchange(true, std::memory_order_relaxed)) {
        char word[19];
        sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_tablestep_not_stock",
                            {{"found", sentinel::HexString(word, found)}, {"expected", "0x20"},
                             {"msg", "the step word is not the game's 0x20, so it is left as the game made it (logged once)"}});
      }
      break;
    case table_step::StepFix::kUnreadable:
      break;
  }
  return obj;
}

NEVR_HOOK_RECORD(kRecord, Thunk, &Handler);

ModuleResult Done(const char* result, const char* reason, const char* status, const Pins& pins, const char* consequence, bool ok) noexcept {
  char slot[19];
  const bool refused = std::strcmp(result, "refused") == 0;
  sentinel::LogFields(refused ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, "combat_tablestep_install",
                      {{"result", result}, {"reason", reason}, {"status", status},
                       {"slot_vaddr", sentinel::HexString(slot, pins.slot)}, {"consequence", consequence}});
  return {ok, status};
}

constexpr const char* kStockConsequence = "shader sets are created by the game's CreateInstance, growth step 32";

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept {
  table_step::WarnLegacySwitch("combat_tablestep_legacy_switch", "tablestep.off", "combat.tablestep: false");
  if (!on) return Done("off", "off_config", "off", pins, "combat.tablestep is false: shader sets are the game's, growth step 32", false);
  if (g_ran.exchange(true, std::memory_order_acq_rel)) return Done("refused", "already_ran", "already_ran", pins, "a second step installs nothing", false);
  sentinel::ElfImage image;
  if (const char* const token = table_step::FindPinnedLibR15(lookup, &image)) return Done("refused", token, token, pins, kStockConsequence, false);
  std::uint32_t have = 0;
  bool readable = false;
  if (const table_step::PinWord* const bad = table_step::FirstPinMismatch(image.base, kWords, kWordCount, &have, &readable)) {
    char at[19];
    char haveText[19];
    char wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_tablestep_pin",
                        {{"vaddr", sentinel::HexString(at, bad->vaddr)},
                         {"have", readable ? sentinel::HexString(haveText, have) : "unreadable"},
                         {"want", sentinel::HexString(wantText, bad->word)},
                         {"msg", "the growth-step instruction is not the pinned build's; the module is not installed"}});
    return Done("refused", "pinned_word_mismatch", "pinned_word_mismatch", pins, kStockConsequence, false);
  }
  table_step::RegisterCounts("combat_tablestep_counts", &CountsOf);
  Thunk::Arm(kRecord);
  const sentinel::GotStatus got = sentinel::InstallThunk<Thunk>(g_hook, Target(image.base, pins), lookup);
  if (got != sentinel::GotStatus::kOk) {
    Thunk::Disarm();
    return Done("refused", sentinel::GotStatusName(got), sentinel::GotStatusName(got), pins, kStockConsequence, false);
  }
  return Done("installed", "ok", "installed", pins, "shader sets created by CreateInstance in a combat match have the table growth step 1; arena keeps the game's object",
              true);
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, on);
}

Stats CurrentStats() noexcept {
  return {g_rewritten.load(std::memory_order_relaxed), g_unexpected.load(std::memory_order_relaxed), g_stock.load(std::memory_order_relaxed)};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  Thunk::Reset();
  g_ran.store(false);
  g_rewritten.store(0);
  g_unexpected.store(0);
  g_stock.store(0);
  g_saidFirst.store(false);
  g_saidUnexpected.store(false);
  g_saidStock.store(false);
}

}  // namespace nevr_quest::combat::tablestep

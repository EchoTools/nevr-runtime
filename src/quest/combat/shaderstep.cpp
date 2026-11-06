#include "quest/combat/shaderstep.h"

#include <atomic>
#include <cstring>

#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::shaderstep {

namespace {

constexpr std::uint32_t kNotStockLines = 5;  // Warnings per process; the count is in Stats

sentinel::GotHook g_hook;
std::atomic<bool> g_ran{false};
std::atomic<std::uint32_t> g_constructed{0};
std::atomic<std::uint32_t> g_stepped{0};
std::atomic<std::uint32_t> g_notStock{0};
std::atomic<std::uint32_t> g_stock{0};
std::atomic<bool> g_saidFirst{false};
std::atomic<bool> g_saidStock{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};

table_step::Counts CountsOf() noexcept {
  return {g_stock.load(std::memory_order_relaxed) + g_notStock.load(std::memory_order_relaxed), g_stepped.load(std::memory_order_relaxed)};
}

void SayStock(table_step::Scope scope) noexcept {
  if (g_saidStock.exchange(true, std::memory_order_relaxed)) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_shaderstep_stock",
                      {{"scope", table_step::ScopeName(scope)},
                       {"msg", "the first shader set built outside a combat match is the game's own, growth step 32"}});
}

// The game's function runs first, for every call; in a combat match the step of the object it built is then corrected. The log is written
// after the work, never while a game call is on the stack above it (callback_thunk.h rule 1).
void* Handler(Thunk::Fn original, void* self, void* obj, const void* init) noexcept {
  const table_step::Scope scope = table_step::CurrentScope();
  void* const built = original(self, obj, init);
  if (scope != table_step::Scope::kCombat) {
    g_stock.fetch_add(1, std::memory_order_relaxed);
    SayStock(scope);
    return built;
  }
  const std::uint32_t n = g_constructed.fetch_add(1, std::memory_order_relaxed) + 1;
  std::uint64_t found = 0;
  switch (ApplyShaderSetStep(built, &found)) {
    case StepFix::kSet:
      g_stepped.fetch_add(1, std::memory_order_relaxed);
      if (!g_saidFirst.exchange(true, std::memory_order_relaxed)) {
        char at[19];
        sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_shaderstep_first",
                            {{"obj", sentinel::HexString(at, reinterpret_cast<std::uintptr_t>(built))}, {"step", table_step::kShaderSetStep},
                             {"n", n}, {"msg", "the first shader set built in a combat match has the table growth step 1"}});
      }
      break;
    case StepFix::kNotStock:
      if (g_notStock.fetch_add(1, std::memory_order_relaxed) < kNotStockLines) {
        char word[19];
        sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_shaderstep_not_stock",
                            {{"found", sentinel::HexString(word, found)}, {"expected", "0x20"},
                             {"msg", "the step word is not the game's 0x20, so it is left as the game made it"}});
      }
      break;
    case StepFix::kUnreadable:
      break;
  }
  return built;
}

NEVR_HOOK_RECORD(kRecord, Thunk, &Handler);

ModuleResult Done(const char* result, const char* reason, const char* status, const Pins& pins, const char* consequence, bool ok) noexcept {
  char slot[19];
  const bool refused = std::strcmp(result, "refused") == 0;
  sentinel::LogFields(refused ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, "combat_shaderstep_install",
                      {{"result", result}, {"reason", reason}, {"status", status},
                       {"slot_vaddr", sentinel::HexString(slot, pins.slot)}, {"consequence", consequence}});
  return {ok, status};
}

constexpr const char* kStockConsequence = "shader sets are built by the game's ConstructInstance, growth step 32";

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept {
  table_step::WarnLegacySwitch("combat_shaderstep_legacy_switch", "shaderstep.off", "combat.shaderstep: false");
  if (!on) return Done("off", "off_config", "off", pins, "combat.shaderstep is false: shader sets are the game's, growth step 32", false);
  if (g_ran.exchange(true, std::memory_order_acq_rel)) return Done("refused", "already_ran", "already_ran", pins, "a second step installs nothing", false);
  sentinel::ElfImage image;
  if (const char* const token = table_step::FindPinnedLibR15(lookup, &image)) return Done("refused", token, token, pins, kStockConsequence, false);
  table_step::RegisterCounts("combat_shaderstep_counts", &CountsOf);
  Thunk::Arm(kRecord);
  const sentinel::GotStatus got = sentinel::InstallThunk<Thunk>(g_hook, Target(image.base, pins), lookup);
  if (got != sentinel::GotStatus::kOk) {
    Thunk::Disarm();
    return Done("refused", sentinel::GotStatusName(got), sentinel::GotStatusName(got), pins, kStockConsequence, false);
  }
  return Done("installed", "ok", "installed", pins, "shader sets built by ConstructInstance in a combat match have the table growth step 1; arena keeps the game's object",
              true);
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, on);
}

Stats CurrentStats() noexcept {
  return {g_constructed.load(std::memory_order_relaxed), g_stepped.load(std::memory_order_relaxed), g_notStock.load(std::memory_order_relaxed),
          g_stock.load(std::memory_order_relaxed)};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  Thunk::Reset();
  g_ran.store(false);
  g_constructed.store(0);
  g_stepped.store(0);
  g_notStock.store(0);
  g_stock.store(0);
  g_saidFirst.store(false);
  g_saidStock.store(false);
}

}  // namespace nevr_quest::combat::shaderstep

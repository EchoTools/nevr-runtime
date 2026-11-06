#include "quest/combat/instmodel.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/table_step.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::instmodel {

namespace {

// The eight CTableXT members, by the offset of their CMemBlock inside the object. Each member's growth step is at member + 0x20 and its
// capacity at member + 0x28 (disassembly 0x1a078dc stores the steps at 0x98, 0xd0, 0x108, 0x140, 0x178, 0x1b0, 0x1e8 and 0x220; each of its
// CMemBlock constructions is at 0x78, 0xb0, ... 0x200).
constexpr std::uintptr_t kMembers[] = {0x78, 0xb0, 0xe8, 0x120, 0x158, 0x190, 0x1c8, 0x200};
constexpr std::uintptr_t kStepOffset = 0x20;
constexpr std::uint64_t kStockStep = 0x20;  // the value the stock constructor stores
constexpr std::uint64_t kNewStep = 1;
constexpr unsigned kLogLines = 16;  // lines per process of each kind; the first over the cap says so, then silence

sentinel::GotHook g_hook;
std::atomic<bool> g_ran{false};
std::atomic<std::uint32_t> g_objects{0};
std::atomic<std::uint32_t> g_stepsSet{0};
std::atomic<std::uint32_t> g_mismatch{0};
std::atomic<std::uint32_t> g_stock{0};
std::atomic<unsigned> g_stepLines{0};
std::atomic<unsigned> g_mismatchLines{0};
std::atomic<bool> g_saidStock{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};

// Sets the growth step of each member whose stock value is still in place. A member that holds another value is left as it is, counted in
// *mismatch, and named in a Warning (the first kLogLines of the process).
unsigned ApplySteps(unsigned char* self, unsigned* mismatch) noexcept {
  unsigned set = 0;
  for (const std::uintptr_t member : kMembers) {
    const std::uintptr_t at = member + kStepOffset;
    std::uint64_t have = 0;
    std::memcpy(&have, self + at, sizeof have);
    if (have != kStockStep) {
      ++*mismatch;
      if (g_mismatchLines.fetch_add(1, std::memory_order_relaxed) < kLogLines) {
        char off[19];
        char found[19];
        sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_instmodel_mismatch",
                            {{"offset", sentinel::HexString(off, at)}, {"found", sentinel::HexString(found, have)}, {"expected", "0x20"},
                             {"msg", "a growth step does not hold the stock value, so it is left as the game made it"}});
      }
      continue;
    }
    const std::uint64_t one = kNewStep;
    std::memcpy(self + at, &one, sizeof one);
    ++set;
  }
  return set;
}

table_step::Counts CountsOf() noexcept { return {g_stock.load(std::memory_order_relaxed), g_objects.load(std::memory_order_relaxed)}; }

void Say(unsigned set, unsigned mismatch) noexcept {
  const unsigned n = g_stepLines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == kLogLines + 1) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_instmodel_log_cap",
                        {{"cap", kLogLines}, {"msg", "further growth steps are set but not logged; combat_instmodel_counts holds the totals for each level"}});
  }
  if (n > kLogLines) return;
  sentinel::LogFields(sentinel::LogLevel::kDebug, "combat_instmodel_steps",
                      {{"steps_set", set}, {"steps_mismatch", mismatch}, {"step", kNewStep},
                       {"msg", "an instanced-model resource built in a combat match has the table growth step 1"}});
}

void SayStock(table_step::Scope scope) noexcept {
  if (g_saidStock.exchange(true, std::memory_order_relaxed)) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_instmodel_stock",
                      {{"scope", table_step::ScopeName(scope)},
                       {"msg", "the first instanced-model resource built outside a combat match is the game's own, growth step 32"}});
}

// The game's constructor runs first, for every call; in a combat match the steps are then corrected. The log is written after the work.
void Handler(Thunk::Fn original, void* self, const void* init) noexcept {
  const table_step::Scope scope = self != nullptr ? table_step::CurrentScope() : table_step::Scope::kUnknown;
  original(self, init);
  if (scope != table_step::Scope::kCombat) {
    g_stock.fetch_add(1, std::memory_order_relaxed);
    SayStock(scope);
    return;
  }
  unsigned mismatch = 0;
  const unsigned set = ApplySteps(static_cast<unsigned char*>(self), &mismatch);
  g_objects.fetch_add(1, std::memory_order_relaxed);
  g_stepsSet.fetch_add(set, std::memory_order_relaxed);
  g_mismatch.fetch_add(mismatch, std::memory_order_relaxed);
  Say(set, mismatch);
}

NEVR_HOOK_RECORD(kRecord, Thunk, &Handler);

ModuleResult Done(const char* result, const char* reason, const char* status, const Pins& pins, const char* consequence, bool ok) noexcept {
  char slot[19];
  const bool refused = std::strcmp(result, "refused") == 0;
  sentinel::LogFields(refused ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, "combat_instmodel_install",
                      {{"result", result}, {"reason", reason}, {"status", status},
                       {"slot_vaddr", sentinel::HexString(slot, pins.slot)}, {"consequence", consequence}});
  return {ok, status};
}

constexpr const char* kStockConsequence = "instanced-model resources are built by the game's constructor, growth step 32";

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept {
  table_step::WarnLegacySwitch("combat_instmodel_legacy_switch", "instmodel.off", "combat.instmodel: false");
  if (!on) return Done("off", "off_config", "off", pins, "combat.instmodel is false: instanced-model resources are the game's, growth step 32", false);
  if (g_ran.exchange(true, std::memory_order_acq_rel)) return Done("refused", "already_ran", "already_ran", pins, "a second step installs nothing", false);
  sentinel::ElfImage image;
  if (const char* const token = table_step::FindPinnedLibR15(lookup, &image)) return Done("refused", token, token, pins, kStockConsequence, false);
  std::uint32_t have = 0;
  bool readable = false;
  if (const table_step::PinWord* const bad = table_step::FirstPinMismatch(image.base, kWords, kWordCount, &have, &readable)) {
    char at[19];
    char haveText[19];
    char wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_instmodel_pin",
                        {{"vaddr", sentinel::HexString(at, bad->vaddr)},
                         {"have", readable ? sentinel::HexString(haveText, have) : "unreadable"},
                         {"want", sentinel::HexString(wantText, bad->word)},
                         {"msg", "a code word the module rests on is not the pinned build's; the module is not installed"}});
    return Done("refused", "pinned_word_mismatch", "pinned_word_mismatch", pins, kStockConsequence, false);
  }
  table_step::RegisterCounts("combat_instmodel_counts", &CountsOf);
  Thunk::Arm(kRecord);
  const sentinel::GotStatus got = sentinel::InstallThunk<Thunk>(g_hook, Target(image.base, pins), lookup);
  if (got != sentinel::GotStatus::kOk) {
    Thunk::Disarm();
    return Done("refused", sentinel::GotStatusName(got), sentinel::GotStatusName(got), pins, kStockConsequence, false);
  }
  return Done("installed", "ok", "installed",
              pins, "instanced-model resources built in a combat match have the table growth step 1; arena keeps the game's object", true);
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, on);
}

Stats CurrentStats() noexcept {
  return {g_objects.load(std::memory_order_relaxed), g_stepsSet.load(std::memory_order_relaxed), g_mismatch.load(std::memory_order_relaxed),
          g_stock.load(std::memory_order_relaxed)};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  Thunk::Reset();
  g_ran.store(false);
  g_objects.store(0);
  g_stepsSet.store(0);
  g_mismatch.store(0);
  g_stock.store(0);
  g_stepLines.store(0);
  g_mismatchLines.store(0);
  g_saidStock.store(false);
}

}  // namespace nevr_quest::combat::instmodel

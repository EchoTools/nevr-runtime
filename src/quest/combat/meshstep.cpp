#include "quest/combat/meshstep.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/safe_read.h"
#include "quest/combat/table_step.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::meshstep {

namespace {

// The growth step the stock constructor stores is 32; this reconstruction stores 1.
constexpr std::uint64_t kGrowStep = 1;
constexpr std::uint64_t kStockGrowStep = 32;

// The member blocks (CMemBlock at each offset, 0x38 bytes) whose growth step the stock constructor writes at block + 0x20. The order is the
// stock constructor's: the CMemBlock constructor, then the step, then the two zeroed words at block + 0x28 and + 0x30.
constexpr std::uintptr_t kTableOffsets[] = {0x2b0, 0x2e8, 0x320, 0x358, 0x390, 0x3c8, 0x400, 0x438, 0x470, 0x4a8, 0x4e0};

sentinel::GotHook g_hook;

// What the handler reads. Written once by the install, before the thunk is armed and the slot swapped; the release store of g_ready
// publishes them, and the handler reads them after an acquire load of it.
Fns g_fns{};
std::uint64_t g_vtPresence = 0;  // NRadEngine::CPresence vtable (the stored word + 0x10)
std::uint64_t g_vtMesh = 0;      // CGMeshListResource vtable (the stored word + 0x10)
std::atomic<bool> g_ready{false};
std::atomic<bool> g_ran{false};
std::atomic<std::uint32_t> g_built{0};
std::atomic<std::uint32_t> g_stock{0};
std::atomic<bool> g_saidBuilt{false};
std::atomic<bool> g_saidStock{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};
std::atomic<const Fns*> g_fnsOverride{nullptr};

void Put64(std::uint8_t* self, std::uintptr_t off, std::uint64_t v) { std::memcpy(self + off, &v, sizeof v); }

std::uint64_t Get64(const std::uint8_t* p, std::uintptr_t off) {
  std::uint64_t v = 0;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

// CGMeshListResource::CGMeshListResource(CPresenceInit const&), the stock order of stores and calls, with the step 1.
void BuildMeshList(std::uint8_t* self, const std::uint8_t* init) {
  Put64(self, 0x10, 0);
  Put64(self, 0x18, 0);
  Put64(self, 0x00, g_vtPresence);
  Put64(self, 0x08, 0);
  Put64(self, 0x20, Get64(init, 0x08));
  Put64(self, 0x28, Get64(init, 0x10));
  Put64(self, 0x30, g_fns.current());
  Put64(self, 0x38, ~std::uint64_t{0});
  Put64(self, 0x40, 0);
  Put64(self, 0x48, 0);
  Put64(self, 0x50, 0);
  Put64(self, 0x58, 0);
  Put64(self, 0x00, g_vtMesh);
  Put64(self, 0x60, 7);
  Put64(self, 0x68, 0);
  g_fns.listDataCtor(self + 0x70);
  for (const std::uintptr_t off : kTableOffsets) {
    g_fns.memblockCtor(self + off, 0, 0, g_fns.currentPtr());
    Put64(self, off + 0x20, kGrowStep);
    Put64(self, off + 0x28, 0);
    Put64(self, off + 0x30, 0);
  }
}

void SayBuilt() noexcept {
  if (g_saidBuilt.exchange(true, std::memory_order_relaxed)) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_meshstep_built",
                      {{"scope", "combat"}, {"growth_step", kGrowStep}, {"stock_growth_step", kStockGrowStep},
                       {"msg", "the first mesh list built in a combat match has the table growth step 1"}});
}

void SayStock(table_step::Scope scope) noexcept {
  if (g_saidStock.exchange(true, std::memory_order_relaxed)) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_meshstep_stock",
                      {{"scope", table_step::ScopeName(scope)},
                       {"msg", "the first mesh list built outside a combat match is the game's own, growth step 32"}});
}

// The thunk's handler: the reconstruction in a combat match, the game's constructor everywhere else. The log is written after the work, never
// while a game call is on the stack above it (callback_thunk.h rule 1).
void Handler(Thunk::Fn original, void* self, const void* init) noexcept {
  const bool ready = g_ready.load(std::memory_order_acquire) && self != nullptr && init != nullptr;
  const table_step::Scope scope = ready ? table_step::CurrentScope() : table_step::Scope::kUnknown;
  if (scope != table_step::Scope::kCombat) {
    original(self, init);
    g_stock.fetch_add(1, std::memory_order_relaxed);
    SayStock(scope);
    return;
  }
  BuildMeshList(static_cast<std::uint8_t*>(self), static_cast<const std::uint8_t*>(init));
  g_built.fetch_add(1, std::memory_order_relaxed);
  SayBuilt();
}

NEVR_HOOK_RECORD(kRecord, Thunk, &Handler);

ModuleResult Done(const char* result, const char* reason, const char* status, const Pins& pins, const char* consequence, bool ok) noexcept {
  char slot[19];
  const bool refused = std::strcmp(result, "refused") == 0;
  sentinel::LogFields(refused ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, "combat_meshstep_install",
                      {{"result", result}, {"reason", reason}, {"status", status},
                       {"slot_vaddr", sentinel::HexString(slot, pins.slot)}, {"consequence", consequence}});
  return {ok, status};
}

constexpr const char* kStockConsequence = "mesh lists are built by the game's constructor, growth step 32";

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Fns* fns, bool on) noexcept {
  table_step::WarnLegacySwitch("combat_meshstep_legacy_switch", "meshstep.off", "combat.meshstep: false");
  if (!on) {
    return Done("off", "off_config", "off", pins, "combat.meshstep is false: mesh lists are built by the game's constructor, growth step 32", false);
  }
  if (g_ran.exchange(true, std::memory_order_acq_rel)) {
    return Done("refused", "already_ran", "already_ran", pins, "a second step installs nothing", false);
  }
  sentinel::ElfImage image;
  if (const char* const token = table_step::FindPinnedLibR15(lookup, &image)) {
    return Done("refused", token, token, pins, kStockConsequence, false);
  }
  std::uint32_t have = 0;
  bool readable = false;
  if (const table_step::PinWord* const bad = table_step::FirstPinMismatch(image.base, kWords, kWordCount, &have, &readable)) {
    char at[19];
    char haveText[19];
    char wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_meshstep_pin",
                        {{"vaddr", sentinel::HexString(at, bad->vaddr)},
                         {"have", readable ? sentinel::HexString(haveText, have) : "unreadable"},
                         {"want", sentinel::HexString(wantText, bad->word)},
                         {"msg", "a code word the reconstruction rests on is not the pinned build's; the module is not installed"}});
    return Done("refused", "pinned_word_mismatch", "pinned_word_mismatch", pins, kStockConsequence, false);
  }
  Fns resolved{};
  if (fns != nullptr) {
    resolved = *fns;
  } else {
    resolved.current = reinterpret_cast<std::uint64_t (*)()>(image.base + pins.current);
    resolved.currentPtr = reinterpret_cast<void* (*)()>(image.base + pins.currentPtr);
    resolved.listDataCtor = reinterpret_cast<void (*)(void*)>(image.base + pins.listDataCtor);
    resolved.memblockCtor = reinterpret_cast<void (*)(void*, std::uint64_t, std::uint64_t, void*)>(image.base + pins.memblockCtor);
  }
  if (resolved.current == nullptr || resolved.currentPtr == nullptr || resolved.listDataCtor == nullptr || resolved.memblockCtor == nullptr) {
    return Done("refused", "helper_missing", "helper_missing", pins, kStockConsequence, false);
  }
  bool okPresence = true;  // SafeRead only ever clears it
  bool okMesh = true;
  const std::uint64_t vp = SafeRead<std::uint64_t>(image.base + pins.vtblPresence, &okPresence);
  const std::uint64_t vm = SafeRead<std::uint64_t>(image.base + pins.vtblMesh, &okMesh);
  if (!okPresence || !okMesh || vp == 0 || vm == 0) {
    return Done("refused", "vtable_unreadable", "vtable_unreadable", pins, kStockConsequence, false);
  }
  g_fns = resolved;
  g_vtPresence = vp + 0x10;
  g_vtMesh = vm + 0x10;
  g_ready.store(true, std::memory_order_release);  // published before the slot can reach the handler
  Thunk::Arm(kRecord);
  const sentinel::GotStatus got = sentinel::InstallThunk<Thunk>(g_hook, Target(image.base, pins), lookup);
  if (got != sentinel::GotStatus::kOk) {
    Thunk::Disarm();
    g_ready.store(false, std::memory_order_release);
    return Done("refused", sentinel::GotStatusName(got), sentinel::GotStatusName(got), pins,
                kStockConsequence, false);
  }
  return Done("installed", "ok", "installed", pins, "mesh lists built in a combat match have the table growth step 1; arena keeps the game's constructor",
              true);
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins,
                   g_fnsOverride.load(std::memory_order_acquire), on);
}

Stats CurrentStats() noexcept { return {g_built.load(std::memory_order_relaxed), g_stock.load(std::memory_order_relaxed)}; }

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }
void SetFnsForTest(const Fns* fns) noexcept { g_fnsOverride.store(fns, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  Thunk::Reset();
  g_ready.store(false);
  g_ran.store(false);
  g_fns = Fns{};
  g_vtPresence = g_vtMesh = 0;
  g_built.store(0);
  g_stock.store(0);
  g_saidBuilt.store(false);
  g_saidStock.store(false);
}

}  // namespace nevr_quest::combat::meshstep

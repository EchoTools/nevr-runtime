#include "quest/combat/memsys.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/mapping_perms.h"

namespace nevr_quest::combat::memsys {

namespace {

namespace pinned = sentinel::pinned;

sentinel::GotHook g_allocHook;
sentinel::GotHook g_initHook;

std::atomic<bool> g_ran{false};
std::atomic<std::uintptr_t> g_base{0};
std::atomic<std::uintptr_t> g_siteAllocOverride{0};
std::atomic<std::uintptr_t> g_siteInitOverride{0};
std::atomic<std::uintptr_t> g_lastAllocCaller{0};
std::atomic<std::uintptr_t> g_lastInitCaller{0};
std::atomic<std::uint64_t> g_allocSiteOffset{kPins.allocSite};
std::atomic<std::uint64_t> g_initSiteOffset{kPins.initSite};
std::atomic<std::uint64_t> g_systemSlotOffset{kPins.systemSlot};
std::atomic<std::uint32_t> g_allocCalls{0};
std::atomic<std::uint32_t> g_poolRaised{0};
std::atomic<std::uint32_t> g_poolNotRaised{0};
std::atomic<std::uint32_t> g_initCalls{0};
std::atomic<bool> g_poolLogged{false};
std::atomic<bool> g_initSeen{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};

constexpr std::uint64_t kStockSystemMiB = 400;

std::uintptr_t SiteAlloc() noexcept {
  const std::uintptr_t o = g_siteAllocOverride.load(std::memory_order_acquire);
  return o != 0 ? o : g_base.load(std::memory_order_acquire) + g_allocSiteOffset.load(std::memory_order_acquire);
}
std::uintptr_t SiteInit() noexcept {
  const std::uintptr_t o = g_siteInitOverride.load(std::memory_order_acquire);
  return o != 0 ? o : g_base.load(std::memory_order_acquire) + g_initSiteOffset.load(std::memory_order_acquire);
}

template <typename T>
bool ReadGame(std::uintptr_t address, T* out) noexcept {
  if (!sentinel::MappingsAllow(address, sizeof(T), /*write=*/false)) return false;
  std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(T));
  return true;
}

// The pool the game's Alloc just made with 0x400 slots: the layout as expected? Then 1023..512 go under the inline fill's 511..0 and the
// count says 512. False (nothing written) on any doubt.
bool RaisePool(std::uintptr_t pool) noexcept {
  std::uintptr_t stack = 0;
  std::uint64_t stackCap = 0;
  std::uint32_t cap = 0;
  std::uint64_t count = 0;
  const bool readable = ReadGame(pool + kOffStack, &stack) && ReadGame(pool + kOffStackCap, &stackCap) &&
                        ReadGame(pool + kOffCap, &cap) && ReadGame(pool + kOffCount, &count);
  if (!readable || stack == 0 || stack % 4 != 0 || stackCap < kCapNew || cap != kCapNew || count != 0 ||
      !sentinel::MappingsAllow(stack, sizeof(std::uint32_t) * kCapNew, /*write=*/true) ||
      !sentinel::MappingsAllow(pool + kOffCount, sizeof(std::uint64_t), /*write=*/true)) {
    if (!g_poolLogged.exchange(true)) {
      char stackText[19];
      sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_memsys_pool",
                          {{"result", "not_raised"}, {"stack", sentinel::HexString(stackText, stack)}, {"stack_cap", stackCap},
                           {"cap_read", cap}, {"count", count}, {"readable", readable ? 1 : 0},
                           {"consequence", "the layout pool is the stock 512 slots; a combat match can run it dry"}});
    }
    return false;
  }
  std::uint32_t* const free = reinterpret_cast<std::uint32_t*>(stack);
  for (std::uint32_t i = 0; i < kCapNew - kCapStock; ++i) free[i] = kCapNew - 1 - i;  // 1023..512, below the inline 511..0
  const std::uint64_t pushed = kCapNew - kCapStock;
  std::memcpy(reinterpret_cast<void*>(pool + kOffCount), &pushed, sizeof(pushed));
  if (!g_poolLogged.exchange(true)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_memsys_pool",
                        {{"result", "raised"}, {"slots", kCapNew}, {"pre_pushed", kCapNew - kCapStock}, {"cap_read_back", cap},
                         {"consequence", "the layout pool holds 1024 render-target layouts once the inline fill has run"}});
  }
  return true;
}

// R37. The original runs for every call; only InitCommon's call with the stock capacity asks for more.
std::uint64_t AllocHandler(AllocThunk::Fn original, const void* caller, void* pool, std::uint32_t capacity, void* allocator) noexcept {
  g_allocCalls.fetch_add(1, std::memory_order_relaxed);
  g_lastAllocCaller.store(reinterpret_cast<std::uintptr_t>(caller), std::memory_order_relaxed);
  if (reinterpret_cast<std::uintptr_t>(caller) != SiteAlloc() || capacity != kCapStock || pool == nullptr) {
    return original(pool, capacity, allocator);
  }
  const std::uint64_t result = original(pool, kCapNew, allocator);
  if (RaisePool(reinterpret_cast<std::uintptr_t>(pool))) {
    g_poolRaised.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_poolNotRaised.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

// android_main's first call: the System heap word the game is about to read. Runs before the game's Init, so no logging frame is live
// while game code runs.
void Witness() noexcept {
  std::uintptr_t word = 0;
  std::uint64_t bytes = 0;
  const std::uintptr_t slot = g_base.load(std::memory_order_acquire) + g_systemSlotOffset.load(std::memory_order_acquire);
  const bool ok = ReadGame(slot, &word) && ReadGame(word, &bytes);
  char slotText[19];
  const std::uint64_t mib = bytes >> 20;
  const bool known = ok && mib != 0;  // a zero size is not a heap the game could be about to use
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_memsys_witness",
                      {{"msg", "android_main reached CSysJava::Init"},
                       known ? sentinel::LogField{"system_heap_mib_next_read", mib}
                             : sentinel::LogField{"system_heap_mib_next_read", "unreadable"},
                       {"system_heap", !known ? "unreadable" : (mib == kStockSystemMiB ? "stock" : "raised")},
                       {"system_slot_vaddr", sentinel::HexString(slotText, g_systemSlotOffset.load(std::memory_order_acquire))}});
}

// R38. The witness is written once, on the call from android_main's site; every call goes on to the game's function.
std::uint64_t InitHandler(InitThunk::Fn original, const void* caller, void* app) noexcept {
  g_initCalls.fetch_add(1, std::memory_order_relaxed);
  g_lastInitCaller.store(reinterpret_cast<std::uintptr_t>(caller), std::memory_order_relaxed);
  if (reinterpret_cast<std::uintptr_t>(caller) == SiteInit() && !g_initSeen.exchange(true)) Witness();
  return original(app);
}

NEVR_HOOK_RECORD(kAllocRecord, AllocThunk, &AllocHandler);
NEVR_HOOK_RECORD(kInitRecord, InitThunk, &InitHandler);

// The first pin whose word is not the live image's, for `row`; nullptr when every word of the row holds. `have` and `readable` say what was read.
const Word* FirstWordMismatch(std::uintptr_t base, int row, std::uint32_t* have, bool* readable) noexcept {
  for (const Word& pin : kWords) {
    if (pin.row != row) continue;
    *have = 0;
    *readable = ReadGame(base + pin.vaddr, have);
    if (!*readable || *have != pin.word) return &pin;
  }
  return nullptr;
}

// One row: its pinned words, then the thunk on its slot. The status is a GotStatus name or `pinned_word_mismatch`.
template <typename Thunk, typename Record>
const char* InstallRow(const char* row, const char* symbol, int rowNumber, std::uintptr_t base, sentinel::GotHook& hook,
                       const sentinel::GotTarget& target, const Record& record, sentinel::ImageLookup lookup) noexcept {
  const char* status = nullptr;
  std::uint32_t have = 0;
  bool readable = false;
  if (const Word* const mismatch = FirstWordMismatch(base, rowNumber, &have, &readable)) {
    char at[19];
    char haveText[19];
    char wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_memsys_hook",
                        {{"row", row}, {"symbol", symbol}, {"status", "pinned_word_mismatch"},
                         {"vaddr", sentinel::HexString(at, mismatch->vaddr)},
                         {"have", readable ? sentinel::HexString(haveText, have) : "unreadable"},
                         {"want", sentinel::HexString(wantText, mismatch->word)},
                         {"consequence", "the row is not installed: its pinned code is not the pinned build's"}});
    return "pinned_word_mismatch";
  }
  Thunk::Arm(record);
  const sentinel::GotStatus got = sentinel::InstallThunk<Thunk>(hook, target, lookup);
  if (got != sentinel::GotStatus::kOk) Thunk::Disarm();
  status = sentinel::GotStatusName(got);
  sentinel::LogFields(got == sentinel::GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_memsys_hook",
                      {{"row", row}, {"symbol", symbol}, {"status", status}});
  return status;
}

ModuleResult Refused(const char* token) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_memsys_install",
                      {{"result", "refused"}, {"reason", token}, {"r37", "not_tried"}, {"r38", "not_tried"},
                       {"consequence", "the layout pool is the stock 512 slots and no witness line is written"}});
  return {false, token};
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept {
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_memsys_install",
                        {{"result", "off"}, {"reason", "combat.memsys"}, {"r37", "off"}, {"r38", "off"},
                         {"consequence", "combat.memsys is false: the layout pool is the stock 512 slots and the System heap is the game's"}});
    return {false, "off"};
  }
  if (g_ran.exchange(true, std::memory_order_acq_rel)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_memsys_install",
                        {{"result", "refused"}, {"reason", "already_ran"}, {"consequence", "a second step installs nothing"}});
    return {false, "already_ran"};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refused("module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id");
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch");

  g_allocSiteOffset.store(pins.allocSite, std::memory_order_release);
  g_initSiteOffset.store(pins.initSite, std::memory_order_release);
  g_systemSlotOffset.store(pins.systemSlot, std::memory_order_release);
  g_base.store(image.base, std::memory_order_release);  // published before either slot can reach a handler

  const char* const r37 = InstallRow<AllocThunk>("R37", kAllocSymbol, 37, image.base, g_allocHook, AllocTarget(image.base, pins),
                                                 kAllocRecord, lookup);
  const char* const r38 = InstallRow<InitThunk>("R38", kInitSymbol, 38, image.base, g_initHook, InitTarget(image.base, pins),
                                                kInitRecord, lookup);
  const bool ok37 = std::strcmp(r37, "ok") == 0;
  const bool ok38 = std::strcmp(r38, "ok") == 0;
  const char* const result = ok37 && ok38 ? "installed" : (ok37 || ok38 ? "partial" : "refused");
  sentinel::LogFields(ok37 && ok38 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_memsys_install",
                      {{"result", result}, {"reason", ok37 && ok38 ? "ok" : "row_refused"}, {"r37", r37}, {"r38", r38},
                       {"consequence", ok37 && ok38 ? "the layout pool is raised to 1024 slots and android_main's start is witnessed"
                                                    : (ok37 ? "the pool is raised but android_main's start is not witnessed"
                                                            : (ok38 ? "the witness is written but the pool is the stock 512 slots"
                                                                    : "the layout pool is the stock 512 slots and no witness line is written"))}});
  return {ok37 && ok38, result};
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, on);
}

Stats CurrentStats() noexcept {
  return {g_allocCalls.load(), g_poolRaised.load(), g_poolNotRaised.load(), g_initCalls.load(), g_initSeen.load() ? 1U : 0U};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }

void SetSitesForTest(std::uintptr_t allocReturn, std::uintptr_t initReturn) noexcept {
  g_siteAllocOverride.store(allocReturn);
  g_siteInitOverride.store(initReturn);
}

void LastCallersForTest(std::uintptr_t* allocReturn, std::uintptr_t* initReturn) noexcept {
  if (allocReturn != nullptr) *allocReturn = g_lastAllocCaller.load();
  if (initReturn != nullptr) *initReturn = g_lastInitCaller.load();
}

void ResetForTest() noexcept {
  g_allocHook.Forget();
  g_initHook.Forget();
  AllocThunk::Reset();
  InitThunk::Reset();
  g_ran.store(false);
  g_base.store(0);
  g_siteAllocOverride.store(0);
  g_siteInitOverride.store(0);
  g_lastAllocCaller.store(0);
  g_lastInitCaller.store(0);
  g_allocSiteOffset.store(kPins.allocSite);
  g_initSiteOffset.store(kPins.initSite);
  g_systemSlotOffset.store(kPins.systemSlot);
  g_allocCalls.store(0);
  g_poolRaised.store(0);
  g_poolNotRaised.store(0);
  g_initCalls.store(0);
  g_poolLogged.store(false);
  g_initSeen.store(false);
}

}  // namespace nevr_quest::combat::memsys

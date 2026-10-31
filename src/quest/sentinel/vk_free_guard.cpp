#include "vk_free_guard.h"

#include <atomic>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::vk_free_guard {

namespace {

sentinel::GotHook g_hook;
std::atomic<const volatile std::uint32_t*> g_count{nullptr};
std::atomic<const vk_hole::Entry*> g_table{nullptr};
std::atomic<std::uint64_t> g_leaked{0};
std::atomic<std::uint64_t> g_corrupt{0};
std::atomic<std::uint32_t> g_highWater{0};
std::atomic<unsigned> g_loggedMarks{0};

// The counts that get one log line the first time the array reaches them.
constexpr std::uint32_t kMarks[] = {128, 256, 384, vk_hole::kGuardFrom};
constexpr unsigned kMarkCount = sizeof(kMarks) / sizeof(kMarks[0]);
// A leak is logged the first time and then once for this many.
constexpr std::uint64_t kLeakLogEvery = 1024;

void NoteCount(std::uint32_t count) noexcept {
  std::uint32_t seen = g_highWater.load(std::memory_order_relaxed);
  while (count > seen && !g_highWater.compare_exchange_weak(seen, count, std::memory_order_relaxed)) {
  }
  for (unsigned i = 0; i < kMarkCount; ++i) {
    if (count < kMarks[i]) break;
    const unsigned bit = 1u << i;
    if ((g_loggedMarks.fetch_or(bit, std::memory_order_relaxed) & bit) != 0) continue;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "vk_hole_table",
                        {{"level", "high_water"}, {"count", count}, {"capacity", vk_hole::kCapacity},
                         {"guard_from", vk_hole::kGuardFrom},
                         {"consequence", "libr15 keeps its free GPU ranges in an array of this size and does not check it"}});
  }
}

void LogLeak(vk_hole::Verdict verdict, std::uint32_t count, void* memory, unsigned long begin, unsigned long end,
             std::uint64_t leaked) noexcept {
  char memoryHex[19];
  char beginHex[19];
  char endHex[19];
  sentinel::LogFields(sentinel::LogLevel::kWarn, "vk_free_leak",
                      {{"verdict", vk_hole::VerdictName(verdict)}, {"count", count}, {"capacity", vk_hole::kCapacity},
                       {"memory", sentinel::HexString(memoryHex, reinterpret_cast<std::uint64_t>(memory))},
                       {"begin", sentinel::HexString(beginHex, begin)}, {"end", sentinel::HexString(endHex, end)},
                       {"leaked", leaked},
                       {"consequence", verdict == vk_hole::Verdict::kLeak
                                           ? "the range stays allocated; running Free would write past the array and crash the next scan"
                                           : "the count is past the array; Free would scan past it"}});
}

// libr15's CGRawInterfaceVK::Free. Below kGuardFrom the cost is one load and two compares.
void Handler(Thunk::Fn original, void* memory, unsigned long begin, unsigned long unused, unsigned long end) noexcept {
  const volatile std::uint32_t* const countWord = g_count.load(std::memory_order_acquire);
  const vk_hole::Entry* const table = g_table.load(std::memory_order_acquire);
  if (countWord != nullptr && table != nullptr) {
    const std::uint32_t count = *countWord;
    NoteCount(count);
    const vk_hole::Verdict verdict = vk_hole::Judge(table, count, reinterpret_cast<std::uint64_t>(memory), begin, end);
    if (verdict != vk_hole::Verdict::kRun) {
      const std::uint64_t leaked = g_leaked.fetch_add(1, std::memory_order_relaxed) + 1;
      if (verdict == vk_hole::Verdict::kCorrupt) g_corrupt.fetch_add(1, std::memory_order_relaxed);
      if (leaked == 1 || leaked % kLeakLogEvery == 0) LogLeak(verdict, count, memory, begin, end, leaked);
      return;
    }
  }
  original(memory, begin, unused, end);
}

NEVR_HOOK_RECORD(kHook, Thunk, &Handler);

bool WordsAt(const unsigned char* base, std::uint64_t vaddr, const std::uint32_t* words, std::size_t count) noexcept {
  return std::memcmp(base + vaddr, words, count * sizeof(std::uint32_t)) == 0;
}

// The name of the first pinned block whose words are not the live image's; nullptr when all hold.
const char* FirstMismatch(const unsigned char* base) noexcept {
  const sentinel::pinned::PinnedFunction& function = sentinel::pinned::kVkFree;
  if (!WordsAt(base, function.vaddr, function.code, function.words)) return function.name;
  const sentinel::pinned::PinnedCode* const blocks[] = {&sentinel::pinned::kVkFreeCountBlock, &sentinel::pinned::kVkFreeAppendBlock,
                                                        &sentinel::pinned::kVkFreeAppendStore};
  for (const sentinel::pinned::PinnedCode* block : blocks) {
    if (!WordsAt(base, block->vaddr, block->code, block->words)) return block->name;
  }
  return nullptr;
}

void Disarm() noexcept {
  g_count.store(nullptr, std::memory_order_release);
  g_table.store(nullptr, std::memory_order_release);
  Thunk::Disarm();
}

}  // namespace

Counts CurrentCounts() noexcept {
  return {g_leaked.load(std::memory_order_acquire), g_corrupt.load(std::memory_order_acquire),
          g_highWater.load(std::memory_order_acquire)};
}

bool InstallAt(unsigned char* base, const sentinel::GotTarget& target) noexcept {
  if (base == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "vk_free_guard_install",
                        {{"result", "refused"}, {"why", "no_image"}, {"slot", target.symbol},
                         {"consequence", "libr15's free of GPU ranges is the game's own and can overflow its array"}});
    return false;
  }
  if (const char* const mismatch = FirstMismatch(base)) {
    sentinel::LogFields(sentinel::LogLevel::kError, "vk_free_guard_install",
                        {{"result", "refused"}, {"why", "pin_mismatch"}, {"pin", mismatch}, {"slot", target.symbol},
                         {"consequence", "libr15's free of GPU ranges is the game's own and can overflow its array"}});
    return false;
  }
  g_count.store(reinterpret_cast<const volatile std::uint32_t*>(base + sentinel::pinned::vk_hole_layout::kCountVaddr),
                std::memory_order_release);
  g_table.store(reinterpret_cast<const vk_hole::Entry*>(base + sentinel::pinned::vk_hole_layout::kTableVaddr),
                std::memory_order_release);
  // The thunk is armed before the slot changes, so its first call already has a handler.
  Thunk::Arm(kHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, target);
  const bool ok = status == sentinel::GotStatus::kOk;
  if (!ok) Disarm();
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "vk_free_guard_install",
                      {{"result", ok ? "installed" : "refused"}, {"status", sentinel::GotStatusName(status)},
                       {"slot", target.symbol}, {"capacity", vk_hole::kCapacity}, {"guard_from", vk_hole::kGuardFrom},
                       {"consequence", ok ? "a free that would overflow libr15's array of free GPU ranges leaks the range instead"
                                          : "libr15's free of GPU ranges is the game's own and can overflow its array"}});
  return ok;
}

bool Install() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    sentinel::LogFields(sentinel::LogLevel::kError, "vk_free_guard_install",
                        {{"result", "refused"}, {"why", "libr15_not_loaded"}, {"status", sentinel::GotStatusName(sentinel::GotStatus::kModuleNotLoaded)}});
    return false;
  }
  return InstallAt(reinterpret_cast<unsigned char*>(image.base), sentinel::pinned::LibR15VkFree());
}

void ArmForTest(const volatile std::uint32_t* count, const vk_hole::Entry* table) noexcept {
  g_count.store(count, std::memory_order_release);
  g_table.store(table, std::memory_order_release);
  Thunk::Arm(kHook);
}

void ResetForTest() noexcept {
  Disarm();
  Thunk::Reset();
  g_leaked.store(0, std::memory_order_relaxed);
  g_corrupt.store(0, std::memory_order_relaxed);
  g_highWater.store(0, std::memory_order_relaxed);
  g_loggedMarks.store(0, std::memory_order_relaxed);
}

}  // namespace nevr_quest::vk_free_guard

#include "vk_alloc_check.h"

#include <atomic>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "vk_free_guard.h"

namespace nevr_quest::vk_alloc_check {

namespace {

sentinel::GotHook g_hook;
std::atomic<ErrorFn> g_error{nullptr};
std::atomic<const volatile std::uint32_t*> g_tableCount{nullptr};
std::atomic<std::uint64_t> g_refused{0};
std::atomic<std::uint64_t> g_zeroCode{0};

// A refusal is logged the first time and then once for this many: an out-of-memory device refuses every texture after it.
constexpr std::uint64_t kRefusalLogEvery = 256;

// The source the game's error record names, and its message: the game logs both through OutputError.
constexpr const char kErrorFile[] = "nevr/quest/sentinel/vk_alloc_check.cpp";
constexpr unsigned kErrorLine = 2262;
constexpr const char kErrorMessage[] = "vkAllocateMemory gave no memory (nevr refused the allocation)";

void LogRefusal(const MemoryRequirements* reqs, unsigned desired, unsigned minimal, unsigned pooled, unsigned pool,
                std::uint64_t refused, std::uint64_t code) noexcept {
  const volatile std::uint32_t* const countWord = g_tableCount.load(std::memory_order_acquire);
  const long long tableCount = countWord != nullptr ? static_cast<long long>(*countWord) : -1;
  const vk_free_guard::Counts guard = vk_free_guard::CurrentCounts();
  char typeBits[19];
  char desiredHex[19];
  char minimalHex[19];
  sentinel::LogFields(sentinel::LogLevel::kWarn, "vk_alloc_refused",
                      {{"size", reqs != nullptr ? reqs->size : 0}, {"alignment", reqs != nullptr ? reqs->alignment : 0},
                       {"type_bits", sentinel::HexString(typeBits, reqs != nullptr ? reqs->memoryTypeBits : 0)},
                       {"desired_flags", sentinel::HexString(desiredHex, desired)},
                       {"minimal_flags", sentinel::HexString(minimalHex, minimal)}, {"pooled", pooled != 0 ? 1 : 0},
                       {"pool", pool}, {"table_count", tableCount}, {"guard_leaked", guard.leaked},
                       {"guard_high_water", guard.high_water}, {"refused", refused}, {"error_code_zero", code == 0 ? 1 : 0},
                       {"consequence", code != 0 ? "the GPU gave no memory; this resource is not created, and the game does not bind it"
                                                 : "the GPU gave no memory and the game's error maker returned 0, so the caller binds it"}});
}

// libr15's CGRawInterfaceVK::Allocate. A success with memory, or the game's own error, costs one compare.
std::uint64_t Handler(Thunk::Fn original, const MemoryRequirements* reqs, unsigned desired, unsigned minimal, unsigned pooled,
                      unsigned pool, AllocatedBase* out) noexcept {
  const std::uint64_t status = original(reqs, desired, minimal, pooled, pool, out);
  if (status != 0 || out == nullptr || out->memory != 0) return status;
  const ErrorFn error = g_error.load(std::memory_order_acquire);
  if (error == nullptr) return status;
  std::memset(out, 0, sizeof(*out));
  const std::uint64_t code = error(kErrorFile, kErrorLine, sentinel::pinned::kVkAllocErrorCode, kErrorMessage);
  const std::uint64_t refused = g_refused.fetch_add(1, std::memory_order_relaxed) + 1;
  if (code == 0) g_zeroCode.fetch_add(1, std::memory_order_relaxed);
  if (refused == 1 || refused % kRefusalLogEvery == 0 || code == 0) LogRefusal(reqs, desired, minimal, pooled, pool, refused, code);
  return code;
}

NEVR_HOOK_RECORD(kHook, Thunk, &Handler);

bool WordsAt(const unsigned char* base, std::uint64_t vaddr, const std::uint32_t* words, std::size_t count) noexcept {
  return std::memcmp(base + vaddr, words, count * sizeof(std::uint32_t)) == 0;
}

// The name of the first pin that is not the live image's; nullptr when all hold.
const char* FirstMismatch(const unsigned char* base) noexcept {
  for (const sentinel::pinned::PinnedFunction* function :
       {&sentinel::pinned::kVkAllocate, &sentinel::pinned::kErrMsgCreateAndAddError}) {
    if (!WordsAt(base, function->vaddr, function->code, function->words)) return function->name;
  }
  std::uint64_t code = 0;
  std::memcpy(&code, base + sentinel::pinned::kVkAllocErrorCodeVaddr, sizeof(code));
  if (code != sentinel::pinned::kVkAllocErrorCode) return "CGRawInterfaceVK::Allocate error symbol";
  return nullptr;
}

void Disarm() noexcept {
  g_error.store(nullptr, std::memory_order_release);
  g_tableCount.store(nullptr, std::memory_order_release);
  Thunk::Disarm();
}

constexpr const char kRefusedConsequence[] =
    "libr15's Allocate is the game's own: a GPU allocation that fails is reported as made, and the bind after it crashes";

}  // namespace

Counts CurrentCounts() noexcept {
  return {g_refused.load(std::memory_order_acquire), g_zeroCode.load(std::memory_order_acquire)};
}

bool InstallAt(unsigned char* base, const sentinel::GotTarget& target) noexcept {
  if (base == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "vk_alloc_check_install",
                        {{"result", "refused"}, {"why", "no_image"}, {"slot", target.symbol}, {"consequence", kRefusedConsequence}});
    return false;
  }
  if (const char* const mismatch = FirstMismatch(base)) {
    sentinel::LogFields(sentinel::LogLevel::kError, "vk_alloc_check_install",
                        {{"result", "refused"}, {"why", "pin_mismatch"}, {"pin", mismatch}, {"slot", target.symbol},
                         {"consequence", kRefusedConsequence}});
    return false;
  }
  g_error.store(reinterpret_cast<ErrorFn>(base + sentinel::pinned::kErrMsgCreateAndAddError.vaddr), std::memory_order_release);
  g_tableCount.store(reinterpret_cast<const volatile std::uint32_t*>(base + sentinel::pinned::vk_hole_layout::kCountVaddr),
                     std::memory_order_release);
  // The thunk is armed before the slot changes, so its first call already has a handler.
  Thunk::Arm(kHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, target);
  const bool ok = status == sentinel::GotStatus::kOk;
  if (!ok) Disarm();
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "vk_alloc_check_install",
                      {{"result", ok ? "installed" : "refused"}, {"status", sentinel::GotStatusName(status)}, {"slot", target.symbol},
                       {"consequence", ok ? "a GPU allocation reported as made with no memory is refused, so nothing binds it"
                                          : kRefusedConsequence}});
  return ok;
}

bool Install() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    sentinel::LogFields(sentinel::LogLevel::kError, "vk_alloc_check_install",
                        {{"result", "refused"}, {"why", "libr15_not_loaded"},
                         {"status", sentinel::GotStatusName(sentinel::GotStatus::kModuleNotLoaded)}});
    return false;
  }
  return InstallAt(reinterpret_cast<unsigned char*>(image.base), sentinel::pinned::LibR15VkAllocate());
}

void ArmForTest(ErrorFn error, const volatile std::uint32_t* tableCount) noexcept {
  g_error.store(error, std::memory_order_release);
  g_tableCount.store(tableCount, std::memory_order_release);
  Thunk::Arm(kHook);
}

void ResetForTest() noexcept {
  Disarm();
  Thunk::Reset();
  g_refused.store(0, std::memory_order_relaxed);
  g_zeroCode.store(0, std::memory_order_relaxed);
}

}  // namespace nevr_quest::vk_alloc_check

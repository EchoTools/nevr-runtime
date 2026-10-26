#include "resource_request_hook.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstring>

#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"

namespace nevr_quest::resource_request {

namespace {

namespace layout = sentinel::pinned::resource_request_layout;
using sentinel::pinned::CResourceManagerOpaque;

sentinel::GotHook g_hook;
// libr15's load base, set by Install: the caller address is logged as a pinned virtual address (address minus base).
std::atomic<std::uint64_t> g_loadBase{0};

// One slot per distinct (type, name, index) seen. A slot is claimed by compare-and-swap on a 64-bit mix of the key
// and kept for the life of the process; two keys that mixed to the same value would share a slot, which can only make
// a line appear later or earlier than its own schedule, never change what the game does.
struct KeySlot {
  std::atomic<std::uint64_t> key{0};  // 0: free
  std::atomic<std::uint64_t> seen{0};
};
KeySlot g_slots[kKeySlots];
std::atomic<std::uint64_t> g_lineTicket{0};  // request lines asked for; the first kMaxLines are written
std::atomic<std::uint64_t> g_unlogged{0};
std::atomic<bool> g_tableFullSaid{false};
std::atomic<bool> g_capSaid{false};

constexpr std::uint64_t Mix(std::uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

std::uint64_t KeyOf(std::uint64_t type, std::uint64_t name, std::uint64_t index) noexcept {
  std::uint64_t h = Mix(type);
  h = Mix(h ^ name);
  h = Mix(h ^ index);
  return h != 0 ? h : 1;
}

// Counts this request against its key and returns how many times the key has now been seen, or 0 when it found no
// slot within kProbeLimit slots of its home.
std::uint64_t Touch(std::uint64_t key) noexcept {
  const std::size_t home = static_cast<std::size_t>(key % kKeySlots);
  for (unsigned probe = 0; probe < kProbeLimit; ++probe) {
    KeySlot& slot = g_slots[(home + probe) % kKeySlots];
    std::uint64_t held = slot.key.load(std::memory_order_acquire);
    if (held == 0 && slot.key.compare_exchange_strong(held, key, std::memory_order_acq_rel)) held = key;
    if (held != key) continue;
    return slot.seen.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  return 0;
}

long long ThreadId() noexcept { return static_cast<long long>(::syscall(SYS_gettid)); }

std::int64_t ReadI64(const unsigned char* request, std::size_t at) noexcept {
  std::int64_t v = 0;
  std::memcpy(&v, request + at, sizeof(v));
  return v;
}
long long ReadU64(const unsigned char* request, std::size_t at) noexcept {
  std::uint64_t v = 0;
  std::memcpy(&v, request + at, sizeof(v));
  return static_cast<long long>(v);
}

// Logs one request when its key's count is a power of two (1, 2, 4, 8 ...) and the line cap is not reached. Runs after
// the game's function returned, on the thread that called it.
void Note(std::uint64_t type, std::uint64_t name, std::uint64_t index, const void* owner, const void* allocator,
          std::uint64_t ret, const void* caller, const unsigned char* request) noexcept {
  const std::uint64_t seen = Touch(KeyOf(type, name, index));
  if (seen == 0) {
    g_unlogged.fetch_add(1, std::memory_order_relaxed);
    if (!g_tableFullSaid.exchange(true, std::memory_order_relaxed)) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "resource_request_table_full",
                          {{"slots", kKeySlots}, {"probe_limit", kProbeLimit}});
    }
    return;
  }
  if ((seen & (seen - 1)) != 0) return;
  if (g_lineTicket.fetch_add(1, std::memory_order_relaxed) >= kMaxLines) {
    g_unlogged.fetch_add(1, std::memory_order_relaxed);
    if (!g_capSaid.exchange(true, std::memory_order_relaxed)) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "resource_request_cap", {{"max_lines", kMaxLines}});
    }
    return;
  }
  const std::uint64_t pinnedCaller =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)) - g_loadBase.load(std::memory_order_relaxed);
  char typeHex[19], nameHex[19], ownerHex[19], retHex[19], callerHex[19];
  const char* const typeText = sentinel::HexString(typeHex, type);
  const char* const nameText = sentinel::HexString(nameHex, name);
  const char* const retText = sentinel::HexString(retHex, ret);
  const char* const callerText = sentinel::HexString(callerHex, pinnedCaller);
  const char* const site = SiteName(pinnedCaller);
  if (ret != 0) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "resource_request_miss",
                        {{"type", typeText},
                         {"name", nameText},
                         {"index", index},
                         {"ret", retText},
                         {"seen", seen},
                         {"site", site},
                         {"caller", callerText},
                         {"tid", ThreadId()}});
    return;
  }
  const char* const ownerText = sentinel::HexString(ownerHex, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(owner)));
  if (request == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "resource_request",
                        {{"type", typeText},
                         {"name", nameText},
                         {"index", index},
                         {"owner", ownerText},
                         {"allocator_set", allocator != nullptr ? 1 : 0},
                         {"ret", retText},
                         {"seen", seen},
                         {"site", site},
                         {"caller", callerText},
                         {"tid", ThreadId()},
                         {"request_null", 1}});
    return;
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "resource_request",
                      {{"type", typeText},
                       {"name", nameText},
                       {"index", index},
                       {"owner", ownerText},
                       {"allocator_set", allocator != nullptr ? 1 : 0},
                       {"ret", retText},
                       {"seen", seen},
                       {"site", site},
                       {"caller", callerText},
                       {"tid", ThreadId()},
                       {"handle", ReadI64(request, layout::kPackageHandle)},
                       {"chunk", ReadI64(request, layout::kChunkIndex)},
                       {"chunk_offset", ReadU64(request, layout::kChunkFileOffset)},
                       {"chunk_compressed", ReadU64(request, layout::kChunkCompressed)},
                       {"chunk_decompressed", ReadU64(request, layout::kChunkDecompressed)},
                       {"offset_in_chunk", ReadU64(request, layout::kOffsetInChunk)},
                       {"size", ReadU64(request, layout::kSize)},
                       {"alignment", ReadU64(request, layout::kAlignment)}});
}

// CResourceManager::FillResourceFileRequest. The game's function runs first, so the request is the filled one; nothing is
// written to it and the result is returned as it came.
std::uint64_t Handler(FillThunk::Fn original, const void* caller, CResourceManagerOpaque* manager, std::uint64_t type,
                      std::uint64_t name, std::uint64_t unused, void* allocator, void* owner, std::uint64_t index,
                      unsigned char* request) noexcept {
  const std::uint64_t ret = original(manager, type, name, unused, allocator, owner, index, request);
  Note(type, name, index, owner, allocator, ret, caller, request);
  return ret;
}

NEVR_HOOK_RECORD(kFillHook, FillThunk, &Handler);

}  // namespace

const char* SiteName(std::uint64_t pinnedReturn) noexcept {
  for (const sentinel::pinned::FillCallSite& site : sentinel::pinned::kFillCallSites) {
    if (site.bl + 4 == pinnedReturn) return site.name;
  }
  return "unknown";
}

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"resource_request_calls", &FillThunk::CallCounter(), sentinel::ReportKind::kCalls},
      {"resource_request_thunk_faults", &FillThunk::FaultCounter(), sentinel::ReportKind::kFaults},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "resource_request_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  return true;
}

bool Install() noexcept {
  sentinel::ElfImage image;
  const bool found = sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image);
  g_loadBase.store(found ? static_cast<std::uint64_t>(image.base) : 0, std::memory_order_relaxed);
  FillThunk::Arm(kFillHook);
  const sentinel::GotStatus status =
      sentinel::InstallThunk<FillThunk>(g_hook, sentinel::pinned::LibR15FillResourceFileRequest());
  const bool ok = status == sentinel::GotStatus::kOk;
  char baseHex[19];
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "resource_request_install",
                      {{"result", ok ? "installed" : "failed"},
                       {"slot", sentinel::GotStatusName(status)},
                       {"load_base", sentinel::HexString(baseHex, g_loadBase.load(std::memory_order_relaxed))}});
  return ok;
}

bool InstallIfCounted(bool countersRegistered) noexcept {
  if (!countersRegistered) {
    sentinel::LogFields(sentinel::LogLevel::kError, "resource_request_install",
                        {{"result", "skipped"}, {"why", "counters_refused"}});
    return false;
  }
  return Install();
}

void ArmForTest(std::uint64_t loadBase) noexcept {
  g_loadBase.store(loadBase, std::memory_order_relaxed);
  FillThunk::Arm(kFillHook);
}

void ResetForTest() noexcept {
  for (KeySlot& slot : g_slots) {
    slot.key.store(0, std::memory_order_relaxed);
    slot.seen.store(0, std::memory_order_relaxed);
  }
  g_lineTicket.store(0, std::memory_order_relaxed);
  g_unlogged.store(0, std::memory_order_relaxed);
  g_tableFullSaid.store(false, std::memory_order_relaxed);
  g_capSaid.store(false, std::memory_order_relaxed);
  g_loadBase.store(0, std::memory_order_relaxed);
}

Totals CurrentTotals() noexcept {
  const unsigned long long ticket = g_lineTicket.load(std::memory_order_relaxed);
  return {ticket < kMaxLines ? ticket : kMaxLines, g_unlogged.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::resource_request

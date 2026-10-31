#include "resource_request_hook.h"

#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"

namespace nevr_quest::resource_request {

namespace {

namespace layout = sentinel::pinned::resource_request_layout;
namespace manifest = sentinel::pinned::resource_manifest_layout;
using sentinel::pinned::CResourceManagerOpaque;

sentinel::GotHook g_hook;
// libr15's load base, set by Install: the caller address is logged as a pinned virtual address (address minus base).
std::atomic<std::uint64_t> g_loadBase{0};
// Whether the request lines and the overlay's lines are written (nevr_build::kIsDev). The overlay itself is not gated on it.
std::atomic<bool> g_logRequests{false};

// The overlay's game functions. Install stores a pointer to g_resolved once every symbol resolved to its pinned address; a
// null pointer leaves every miss to the game's own function.
std::atomic<const GameFns*> g_fns{nullptr};
GameFns g_resolved{};

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

// The overlay's counters and its line cap (kOverlayLines, shared by the served and the failed lines).
std::atomic<std::uint64_t> g_overlayServed{0};
std::atomic<std::uint64_t> g_overlayFailed{0};
std::atomic<std::uint64_t> g_overlayTicket{0};
std::atomic<bool> g_overlayCapSaid{false};

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
std::uint64_t LoadWord(const unsigned char* base, std::size_t at) noexcept {
  std::uint64_t v = 0;
  std::memcpy(&v, base + at, sizeof(v));
  return v;
}
const unsigned char* LoadPointer(const unsigned char* base, std::size_t at) noexcept {
  std::uintptr_t v = 0;
  std::memcpy(&v, base + at, sizeof(v));
  return reinterpret_cast<const unsigned char*>(v);
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

// True when the manifest the manager holds has no record for (type, name): the same binary search the game runs
// (0x2543a30-0x2543a90), with signed 64-bit keys and an unsigned count. A null manager or manifest answers false, so the
// game's function runs and decides.
bool ManifestMisses(const CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name) noexcept {
  if (manager == nullptr) return false;
  const unsigned char* const table = LoadPointer(reinterpret_cast<const unsigned char*>(manager), manifest::kManagerManifest);
  if (table == nullptr) return false;
  const unsigned char* const records = LoadPointer(table, manifest::kManifestRecords);
  if (records == nullptr) return false;
  const std::int64_t wantType = static_cast<std::int64_t>(type);
  const std::int64_t wantName = static_cast<std::int64_t>(name);
  std::uint64_t lo = 0;
  std::uint64_t hi = LoadWord(table, manifest::kManifestCount);
  while (lo < hi) {
    const std::uint64_t mid = lo + ((hi - lo) >> 1);
    const unsigned char* const rec = records + (mid << manifest::kRecordShift);
    const std::int64_t t = ReadI64(rec, manifest::kRecordType);
    if (t == wantType) {
      const std::int64_t n = ReadI64(rec, manifest::kRecordName);
      if (n == wantName) return false;
      if (n < wantName) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    } else if (t < wantType) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return true;
}

enum class Stage : std::uint8_t { kServed, kPath, kOpen, kSize };

struct Served {
  Stage stage;
  int ret;                  // the failing stage's return value (0 when served)
  unsigned long long size;  // the served file's size
  long handle;              // the served file's handle, as the request holds it
};

const char* StageName(Stage stage) noexcept {
  if (stage == Stage::kPath) return "path";
  if (stage == Stage::kOpen) return "open";
  if (stage == Stage::kSize) return "size";
  return "served";
}

// The loose-file overlay, in the reference build's order (qcp_patches.py P9, fill_request_trampoline): clear the request, store
// the owner and the index, the path, the open (flags 1, O_RDONLY, the handle written into the request), the size, then the
// size words, the allocator and the valid word. Any stage that returns nonzero stops here and reports which. The file is not
// closed (the reference build does not close it either; docs/adr/0018-quest-resource-request-log.md).
// Frame rule: no destructors, no std::string, no exceptions in this function or anything it calls.
Served ServeLoose(const GameFns& fns, CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name,
                  void* allocator, void* owner, std::uint64_t index, unsigned char* request) noexcept {
  Served out{Stage::kServed, 0, 0, 0};
  // The only writes to the request in this file: a copy of each field's bytes at its offset, inside this function.
  const auto put = [request](std::size_t at, const auto& value) noexcept { std::memcpy(request + at, &value, sizeof(value)); };
  fns.clearRequest(request);
  put(layout::kOwner, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(owner)));
  put(layout::kIndex, index);
  out.ret = fns.unpackagedPath(manager, type, name, request + layout::kLoosePath);
  if (out.ret != 0) {
    out.stage = Stage::kPath;
    return out;
  }
  out.ret = fns.open(reinterpret_cast<const char*>(request + layout::kLoosePath), 1,
                     reinterpret_cast<long*>(request + layout::kPackageHandle));
  if (out.ret != 0) {
    out.stage = Stage::kOpen;
    return out;
  }
  out.handle = static_cast<long>(ReadI64(request, layout::kPackageHandle));
  out.ret = fns.size(out.handle, &out.size);
  if (out.ret != 0) {
    out.stage = Stage::kSize;
    return out;
  }
  put(layout::kSize, static_cast<std::uint64_t>(out.size));
  put(layout::kUncompressedSize, static_cast<std::uint64_t>(out.size));
  put(layout::kMemSpace, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(allocator)));
  put(layout::kValid, std::uint32_t{1});
  return out;
}

// Counts an overlay outcome and, in a development build, writes its line: one per served or failed stage, at most
// kOverlayLines lines in all, then one cap line.
void NoteOverlay(const Served& s, std::uint64_t type, std::uint64_t name, std::uint64_t index, const void* owner,
                 const void* caller) noexcept {
  const bool served = s.stage == Stage::kServed;
  if (served) {
    g_overlayServed.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_overlayFailed.fetch_add(1, std::memory_order_relaxed);
  }
  if (!g_logRequests.load(std::memory_order_relaxed)) return;
  if (g_overlayTicket.fetch_add(1, std::memory_order_relaxed) >= kOverlayLines) {
    if (!g_overlayCapSaid.exchange(true, std::memory_order_relaxed)) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "resource_overlay_cap",
                          {{"max_lines", kOverlayLines},
                           {"counted_in", "resource_overlay_served,resource_overlay_failed"}});
    }
    return;
  }
  const std::uint64_t pinnedCaller =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(caller)) - g_loadBase.load(std::memory_order_relaxed);
  char typeHex[19], nameHex[19], ownerHex[19], callerHex[19];
  const char* const typeText = sentinel::HexString(typeHex, type);
  const char* const nameText = sentinel::HexString(nameHex, name);
  const char* const ownerText = sentinel::HexString(ownerHex, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(owner)));
  const char* const callerText = sentinel::HexString(callerHex, pinnedCaller);
  const char* const site = SiteName(pinnedCaller);
  if (served) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "resource_overlay_served",
                        {{"type", typeText},
                         {"name", nameText},
                         {"index", index},
                         {"owner", ownerText},
                         {"size", s.size},
                         {"handle", s.handle},
                         {"site", site},
                         {"caller", callerText},
                         {"tid", ThreadId()}});
    return;
  }
  sentinel::LogFields(sentinel::LogLevel::kWarn, "resource_overlay_failed",
                      {{"type", typeText},
                       {"name", nameText},
                       {"index", index},
                       {"owner", ownerText},
                       {"site", site},
                       {"caller", callerText},
                       {"tid", ThreadId()},
                       {"stage", StageName(s.stage)},
                       {"ret", s.ret},
                       {"consequence", "the game raises its own Cannot find resource"}});
}

// CResourceManager::FillResourceFileRequest. On a miss the overlay may serve the request (then the game's function is not
// called and the result is 0). Otherwise, and on a hit, the game's function runs first and the request it filled is
// returned as it came; the request is written only by the overlay. The lines are written after the game's function
// returned, so no logging frame is live while game code runs.
std::uint64_t Handler(FillThunk::Fn original, const void* caller, CResourceManagerOpaque* manager, std::uint64_t type,
                      std::uint64_t name, std::uint64_t unused, void* allocator, void* owner, std::uint64_t index,
                      unsigned char* request) noexcept {
  if (request != nullptr) {
    const GameFns* const fns = g_fns.load(std::memory_order_acquire);
    if (fns != nullptr && ManifestMisses(manager, type, name)) {
      const Served served = ServeLoose(*fns, manager, type, name, allocator, owner, index, request);
      NoteOverlay(served, type, name, index, owner, caller);
      if (served.stage == Stage::kServed) return 0;
      // Not served: the game's function below raises its own error, as it does without the overlay.
    }
  }
  const std::uint64_t ret = original(manager, type, name, unused, allocator, owner, index, request);
  if (g_logRequests.load(std::memory_order_relaxed)) Note(type, name, index, owner, allocator, ret, caller, request);
  return ret;
}

NEVR_HOOK_RECORD(kFillHook, FillThunk, &Handler);

// The production lookup: the symbol in the loaded libr15.so, or nullptr.
void* LookupLibR15Symbol(const char* symbol) noexcept {
  void* const handle = ::dlopen(sentinel::pinned::kLibR15, RTLD_NOW | RTLD_NOLOAD);
  return handle != nullptr ? ::dlsym(handle, symbol) : nullptr;
}

bool InstallImpl(bool logRequests, const char* counters) noexcept {
  sentinel::ElfImage image;
  const bool found = sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image);
  const std::uint64_t base = found ? static_cast<std::uint64_t>(image.base) : 0;
  g_loadBase.store(base, std::memory_order_relaxed);
  const char* overlay = "module_not_loaded";
  if (found) {
    const char* const refused = ResolveGameFnsForTest(base, &LookupLibR15Symbol, &g_resolved);
    if (refused == nullptr) {
      g_fns.store(&g_resolved, std::memory_order_release);
      overlay = "ready";
    } else {
      g_fns.store(nullptr, std::memory_order_release);
      overlay = refused;
    }
  } else {
    g_fns.store(nullptr, std::memory_order_release);
  }
  g_logRequests.store(logRequests, std::memory_order_release);
  FillThunk::Arm(kFillHook);
  const sentinel::GotStatus status =
      sentinel::InstallThunk<FillThunk>(g_hook, sentinel::pinned::LibR15FillResourceFileRequest());
  const bool ok = status == sentinel::GotStatus::kOk;
  char baseHex[19];
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "resource_request_install",
                      {{"result", ok ? "installed" : "failed"},
                       {"slot", sentinel::GotStatusName(status)},
                       {"load_base", sentinel::HexString(baseHex, base)},
                       {"overlay", overlay},
                       {"log", logRequests ? 1 : 0},
                       {"counters", counters}});
  return ok;
}

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
      {"resource_overlay_served", &g_overlayServed, sentinel::ReportKind::kCalls},
      {"resource_overlay_failed", &g_overlayFailed, sentinel::ReportKind::kCalls},
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

bool Install(bool logRequests) noexcept { return InstallImpl(logRequests, "registered"); }

bool InstallCounted(bool countersRegistered, bool logRequests) noexcept {
  return InstallImpl(logRequests, countersRegistered ? "registered" : "refused");
}

const char* ResolveGameFnsForTest(std::uint64_t base, SymbolLookup lookup, GameFns* out) noexcept {
  struct Want {
    const char* symbol;
    std::uint64_t vaddr;
  };
  const Want wants[4] = {
      {sentinel::pinned::kClearFileRequestDataSymbol, sentinel::pinned::kClearFileRequestDataVaddr},
      {sentinel::pinned::kGetUnpackagedResourcePathSymbol, sentinel::pinned::kGetUnpackagedResourcePathVaddr},
      {sentinel::pinned::kCSysFileOpenSymbol, sentinel::pinned::kCSysFileOpenVaddr},
      {sentinel::pinned::kCSysFileSizeSymbol, sentinel::pinned::kCSysFileSizeVaddr},
  };
  void* resolved[4] = {nullptr, nullptr, nullptr, nullptr};
  const char* refused = nullptr;
  for (std::size_t i = 0; i < 4; ++i) {
    resolved[i] = lookup(wants[i].symbol);
    const std::uint64_t expected = base + wants[i].vaddr;
    const std::uint64_t actual = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(resolved[i]));
    if (resolved[i] != nullptr && actual == expected) continue;
    char expectedHex[19], actualHex[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "resource_overlay_resolve",
                        {{"result", "refused"},
                         {"symbol", wants[i].symbol},
                         {"expected", sentinel::HexString(expectedHex, expected)},
                         {"actual", sentinel::HexString(actualHex, actual)}});
    if (refused == nullptr) refused = wants[i].symbol;
  }
  if (refused != nullptr) return refused;
  out->clearRequest = reinterpret_cast<void (*)(unsigned char*)>(resolved[0]);
  out->unpackagedPath = reinterpret_cast<int (*)(const CResourceManagerOpaque*, std::uint64_t, std::uint64_t, unsigned char*)>(
      resolved[1]);
  out->open = reinterpret_cast<int (*)(const char*, unsigned, long*)>(resolved[2]);
  out->size = reinterpret_cast<int (*)(long, unsigned long long*)>(resolved[3]);
  return nullptr;
}

void ArmForTest(std::uint64_t loadBase, bool logRequests) noexcept {
  g_loadBase.store(loadBase, std::memory_order_relaxed);
  g_logRequests.store(logRequests, std::memory_order_relaxed);
  FillThunk::Arm(kFillHook);
}

void SetGameFnsForTest(const GameFns* fns) noexcept { g_fns.store(fns, std::memory_order_release); }

void ResetForTest() noexcept {
  for (KeySlot& slot : g_slots) {
    slot.key.store(0, std::memory_order_relaxed);
    slot.seen.store(0, std::memory_order_relaxed);
  }
  g_lineTicket.store(0, std::memory_order_relaxed);
  g_unlogged.store(0, std::memory_order_relaxed);
  g_tableFullSaid.store(false, std::memory_order_relaxed);
  g_capSaid.store(false, std::memory_order_relaxed);
  g_overlayServed.store(0, std::memory_order_relaxed);
  g_overlayFailed.store(0, std::memory_order_relaxed);
  g_overlayTicket.store(0, std::memory_order_relaxed);
  g_overlayCapSaid.store(false, std::memory_order_relaxed);
  g_fns.store(nullptr, std::memory_order_release);
  g_logRequests.store(false, std::memory_order_relaxed);
  g_loadBase.store(0, std::memory_order_relaxed);
}

Totals CurrentTotals() noexcept {
  const unsigned long long ticket = g_lineTicket.load(std::memory_order_relaxed);
  return {ticket < kMaxLines ? ticket : kMaxLines, g_unlogged.load(std::memory_order_relaxed)};
}

OverlayTotals CurrentOverlayTotals() noexcept {
  return {g_overlayServed.load(std::memory_order_relaxed), g_overlayFailed.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::resource_request

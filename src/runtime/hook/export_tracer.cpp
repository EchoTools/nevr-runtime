#include "runtime/hook/export_tracer.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "core/logging.h"
#include "runtime/hook/export_trace_policy.h"
#include "runtime/hook/export_trace_thunk.h"

namespace {

namespace Policy = ExportTracePolicy;

constexpr std::size_t kNameBytes = 48;

struct Slot {
  char name[kNameBytes];
  std::uint32_t moduleBit;
  void* original;
  void* thunk;
};

// Static storage, no dynamic initializer: a tracer that is off costs nothing at DLL load.
Slot g_slots[ExportTrace::kMaxThunks];
std::uint32_t g_slotCount = 0;
std::atomic<std::uint32_t> g_mask{0};
std::atomic<bool> g_configured{false};
HANDLE g_stopEvent = nullptr;
HANDLE g_thread = nullptr;

std::mutex& SlotMutex() {
  static std::mutex* const m = new std::mutex();
  return *m;
}

// Per-export totals, touched only by the drain thread.
struct Stats {
  std::uint64_t calls;
  std::uint64_t totalTicks;
  std::uint64_t maxTicks;
  std::uint64_t lastRet;
};
Stats g_stats[ExportTrace::kMaxThunks];

std::uint64_t Rdtsc() {
  std::uint32_t lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

std::uint64_t QpcNs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<std::uint64_t>(static_cast<double>(now.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart));
}

// Counter ticks to nanoseconds, from the span since the tracer started (the counter's rate is not assumed).
struct Clock {
  std::uint64_t tsc0;
  std::uint64_t ns0;
  double NsPerTick() const {
    const std::uint64_t dTsc = Rdtsc() - tsc0;
    const std::uint64_t dNs = QpcNs() - ns0;
    if (dTsc < 1000000 || dNs == 0) return 1.0;
    return static_cast<double>(dNs) / static_cast<double>(dTsc);
  }
};

void LogCall(const nevr::CallRecord& r, std::uint64_t index, double nsPerTick) {
  const Slot& slot = g_slots[r.exportId];
  Log(EchoVR::LogLevel::Info,
      "[NEVR.TRACE] call #%llu module=%s export=%s tid=%u dur_ns=%llu args=%llx,%llx,%llx,%llx ret=%llx xmm0=%llx",
      static_cast<unsigned long long>(index), Policy::ModuleName(slot.moduleBit), slot.name, r.threadId,
      static_cast<unsigned long long>(static_cast<double>(r.exitTicks - r.enterTicks) * nsPerTick),
      static_cast<unsigned long long>(r.args[0]), static_cast<unsigned long long>(r.args[1]),
      static_cast<unsigned long long>(r.args[2]), static_cast<unsigned long long>(r.args[3]),
      static_cast<unsigned long long>(r.ret), static_cast<unsigned long long>(r.retXmm0));
}

void LogSummary(std::uint32_t slotCount, double nsPerTick) {
  for (std::uint32_t id = 0; id < slotCount; ++id) {
    const Stats& s = g_stats[id];
    if (s.calls == 0) continue;
    const Slot& slot = g_slots[id];
    Log(EchoVR::LogLevel::Info,
        "[NEVR.TRACE] summary module=%s export=%s calls=%llu total_dur_ns=%llu max_dur_ns=%llu last_ret=%llx",
        Policy::ModuleName(slot.moduleBit), slot.name, static_cast<unsigned long long>(s.calls),
        static_cast<unsigned long long>(static_cast<double>(s.totalTicks) * nsPerTick),
        static_cast<unsigned long long>(static_cast<double>(s.maxTicks) * nsPerTick),
        static_cast<unsigned long long>(s.lastRet));
  }
}

DWORD WINAPI DrainMain(LPVOID) {
  Clock clock{Rdtsc(), QpcNs()};
  std::uint64_t lastSummaryNs = QpcNs();
  std::uint64_t lastDropped = 0;
  for (;;) {
    const DWORD wait = WaitForSingleObject(g_stopEvent, 250);
    nevr::CallRecord r;
    const double nsPerTick = clock.NsPerTick();
    while (ExportTrace::Pop(&r)) {
      if (r.exportId >= ExportTrace::kMaxThunks) continue;
      Stats& s = g_stats[r.exportId];
      const std::uint64_t ticks = r.exitTicks - r.enterTicks;
      if (s.calls < Policy::kFullLogCalls) LogCall(r, s.calls + 1, nsPerTick);
      ++s.calls;
      s.totalTicks += ticks;
      if (ticks > s.maxTicks) s.maxTicks = ticks;
      s.lastRet = r.ret;
    }
    const std::uint64_t dropped = ExportTrace::Dropped();
    if (dropped != lastDropped) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.TRACE] dropped count=%llu (the ring overflowed between drains)",
          static_cast<unsigned long long>(dropped));
      lastDropped = dropped;
    }
    const std::uint64_t nowNs = QpcNs();
    if (wait == WAIT_OBJECT_0 || nowNs - lastSummaryNs >= Policy::kSummarySeconds * 1000000000ULL) {
      lastSummaryNs = nowNs;
      std::uint32_t count;
      {
        std::lock_guard<std::mutex> lock(SlotMutex());
        count = g_slotCount;
      }
      LogSummary(count, nsPerTick);
    }
    if (wait == WAIT_OBJECT_0) return 0;
  }
}

// True when `address` is in committed, executable memory: a function. A data export (a variable or a
// table the game reads through the pointer) is in a non-executable page, and a thunk there would hand the
// game a stub where it expects the value.
bool PointsToCodeImpl(const void* address) {
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
  if (info.State != MEM_COMMIT) return false;
  const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (info.Protect & executable) != 0;
}

}  // namespace

namespace ExportTracer {

void ConfigureFromCommandLineText(const wchar_t* commandLine) {
  char list[64];
  if (!Policy::ExtractFlagValue(commandLine, L"-traceexports", list, sizeof(list))) list[0] = '\0';
  Configure(list);
}

void ConfigureFromCommandLine() { ConfigureFromCommandLineText(GetCommandLineW()); }

void Shutdown() {
  if (g_thread == nullptr || g_stopEvent == nullptr) return;
  SetEvent(g_stopEvent);
  // The drain thread makes a last pass (the ring, then the summary) when the event is set and returns.
  WaitForSingleObject(g_thread, 2000);
  CloseHandle(g_thread);
  g_thread = nullptr;
}

void Configure(const char* list) {
  bool expected = false;
  if (!g_configured.compare_exchange_strong(expected, true)) return;
  bool unknown = false;
  const std::uint32_t mask = Policy::ParseModules(list, &unknown);
  if (unknown) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.TRACE] -traceexports names a module that is not traced (pnsrad, pnsovr, pnsdemo, all): %s",
        list != nullptr ? list : "");
  }
  if (mask == 0) return;
  ExportTrace::Reset();
  g_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  g_thread = g_stopEvent != nullptr ? CreateThread(nullptr, 0, &DrainMain, nullptr, 0, nullptr) : nullptr;
  if (g_thread == nullptr) {
    Log(EchoVR::LogLevel::Error, "[NEVR.TRACE] the drain thread could not start; the tracer stays off");
    return;
  }
  g_mask.store(mask, std::memory_order_release);
  Log(EchoVR::LogLevel::Info, "[NEVR.TRACE] enabled modules=%s%s%s", (mask & Policy::kPnsrad) ? "pnsrad " : "",
      (mask & Policy::kPnsovr) ? "pnsovr " : "", (mask & Policy::kPnsdemo) ? "pnsdemo" : "");
}

bool PointsToCode(const void* address) { return PointsToCodeImpl(address); }

bool Enabled() { return g_mask.load(std::memory_order_acquire) != 0; }

void* WrapSymbol(void* dllHandle, const char* symbol, void* resolved) {
  const std::uint32_t mask = g_mask.load(std::memory_order_acquire);
  if (mask == 0 || resolved == nullptr || symbol == nullptr || dllHandle == nullptr) return resolved;
  char path[MAX_PATH];
  const DWORD len = GetModuleFileNameA(static_cast<HMODULE>(dllHandle), path, sizeof(path));
  if (len == 0 || len >= sizeof(path)) return resolved;
  const std::uint32_t bit = Policy::ModuleOfPath(path);
  if ((bit & mask) == 0) return resolved;
  if (std::strlen(symbol) >= kNameBytes) return resolved;
  if (!PointsToCodeImpl(resolved)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.TRACE] skip module=%s export=%s: not code (a data export is returned as it is)",
        Policy::ModuleName(bit), symbol);
    return resolved;
  }

  void* thunk = nullptr;
  std::uint32_t id = 0;
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(SlotMutex());
    for (std::uint32_t i = 0; i < g_slotCount; ++i) {
      if (g_slots[i].moduleBit == bit && g_slots[i].original == resolved && std::strcmp(g_slots[i].name, symbol) == 0) {
        return g_slots[i].thunk;
      }
    }
    if (g_slotCount >= ExportTrace::kMaxThunks) return resolved;
    id = g_slotCount;
    thunk = ExportTrace::MakeThunk(resolved, id);
    if (thunk == nullptr) return resolved;
    Slot& slot = g_slots[id];
    std::strncpy(slot.name, symbol, kNameBytes - 1);
    slot.name[kNameBytes - 1] = '\0';
    slot.moduleBit = bit;
    slot.original = resolved;
    slot.thunk = thunk;
    ++g_slotCount;
    fresh = true;
  }
  if (fresh) {
    Log(EchoVR::LogLevel::Info, "[NEVR.TRACE] resolve module=%s export=%s id=%u original=%p", Policy::ModuleName(bit),
        symbol, id, resolved);
  }
  return thunk;
}

}  // namespace ExportTracer

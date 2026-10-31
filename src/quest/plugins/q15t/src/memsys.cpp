#include "memsys.h"

#include <atomic>
#include <cstdio>

#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

constexpr std::uint32_t kCapStock = 0x200, kCapNew = 0x400;
// CSimplePool<SGRenderTargetLayout> layout, read from Alloc's body (disassembly 0x1a01b5c..0x1a01c80):
//   +0x28 free-stack pointer (str x0,[x19,#0x28] at 0x1a01c4c), +0x30 free count (zeroed by stp q0,q0,[x22] at 0x1a01c24,
//   x22 = pool+0x20), +0x38 free-stack capacity in entries (str x21,[x19,#0x38] at 0x1a01c30), +0x84 capacity (str w21,
//   [x19,#0x84] at 0x1a01bbc).
constexpr std::uintptr_t kOffStack = 0x28, kOffCount = 0x30, kOffStackCap = 0x38, kOffCap = 0x84;

constexpr MemsysPins kDefaultPins = {0x36e9b48, 0x1901b5c, 0x18818cc, 0x36de5c0, 0xf7e8e8, 0xf784e8, 0x371d020};

// the source build's memsys:65-77, each word re-read from the disassembly (all 21 match; docs/quest/q15t/modules/memsys.md). `row` is the row refused when
// the word differs.
constexpr MemsysWord kWords[] = {
    {0x18818b8, 0x52804001, 37},  // InitCommon: mov w1,#0x200
    {0x18818c8, 0x97da8bfe, 37},  // bl <PLT Alloc>
    {0x18818cc, 0xa942ae6a, 37},  // ldp x10,x11,[x19,#0x28]  (the return address of that call)
    {0x18818d8, 0x52803f6c, 37},  // mov w12,#0x1fb
    {0x18818e0, 0x4f00c420, 37},  // movi v0.4s,#0x1,msl #8 (0x1ff)
    {0x18818e8, 0x91080169, 37},  // add x9,x11,#0x200 (the free count the inline fill stores)
    {0x1881904, 0xf120011f, 37},  // cmp x8,#0x800 (512 x 4 B)
    {0x1881914, 0xf9001a69, 37},  // str x9,[x19,#0x30]
    {0xf248c0, 0xb0013e30, 37},   // PLT stub Alloc: adrp x16,0x36e9000
    {0xf248c4, 0xf945a611, 37},   // ldr x17,[x16,#0xb48]
    {0x1901b5c, 0xd10143ff, 37},  // Alloc: sub sp,sp,#0x50
    {0x1901c24, 0xad0002c0, 37},  // stp q0,q0,[x22]  (count 0)
    {0x1901c30, 0xf9001e75, 37},  // str x21,[x19,#0x38]  (stack cap)
    {0x1901c4c, 0xf9001660, 37},  // str x0,[x19,#0x28]  (stack)
    {0xf784e4, 0x97fe5633, 38},   // android_main: bl <PLT Init> (first call)
    {0xf784e8, 0x97fdb586, 38},   // bl <PLT CSymbol64::Initialize> (the return address of that call)
    {0xf0ddb0, 0xb0013e90, 38},   // PLT stub Init: adrp x16,0x36de000
    {0xf0ddb4, 0xf942e211, 38},   // ldr x17,[x16,#0x5c0]
    {0xf78510, 0xb0013d28, 38},   // android_main: adrp x8,0x371d000
    {0xf78514, 0xf9401108, 38},   // ldr x8,[x8,#0x20]  (the System GLOB_DAT word)
    {0xf78524, 0xf9400101, 38},   // ldr x1,[x8]  (the size, once)
};
constexpr int kWordCount = static_cast<int>(sizeof kWords / sizeof kWords[0]);

using AllocFn = std::uint64_t (*)(void* pool, std::uint32_t cap, void* allocator);
using InitFn = std::uint64_t (*)(void* app);

MemsysPins g_pins = kDefaultPins;
std::uintptr_t g_base = 0;
std::atomic<std::uintptr_t> g_alloc_real{0}, g_init_real{0};
std::atomic<std::uintptr_t> g_site_alloc{0}, g_site_init{0};  // test overrides; 0 = base + pin
std::atomic<std::uintptr_t> g_last_alloc_ret{0}, g_last_init_ret{0};
std::atomic<std::uint32_t> g_alloc_calls{0}, g_pool_raised{0}, g_pool_not_raised{0}, g_init_calls{0};
std::atomic<bool> g_pool_logged{false}, g_init_seen{false};

std::uintptr_t SiteAlloc() {
  const std::uintptr_t o = g_site_alloc.load(std::memory_order_relaxed);
  return o != 0 ? o : g_base + g_pins.site_ret;
}
std::uintptr_t SiteInit() {
  const std::uintptr_t o = g_site_init.load(std::memory_order_relaxed);
  return o != 0 ? o : g_base + g_pins.init_ret;
}

// The pool the real Alloc just made with 0x400 slots: layout as expected? Then 1023..512 go under the inline fill's
// 511..0 and the count says 512. Returns false (nothing written) on any doubt.
bool RaisePool(std::uintptr_t pool) {
  bool ok = true;
  const auto stack = Rd<std::uintptr_t>(pool + kOffStack, &ok);
  const auto stack_cap = Rd<std::uint64_t>(pool + kOffStackCap, &ok);
  const auto cap = Rd<std::uint32_t>(pool + kOffCap, &ok);
  const auto count = Rd<std::uint64_t>(pool + kOffCount, &ok);
  if (!ok || stack == 0 || stack % 4 != 0 || stack_cap < kCapNew || cap != kCapNew || count != 0) {
    if (!g_pool_logged.exchange(true)) {
      char a[24], b[24], c[24], d[24];
      std::snprintf(a, sizeof a, "0x%llx", static_cast<unsigned long long>(stack));
      std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(stack_cap));
      std::snprintf(c, sizeof c, "%u", cap);
      std::snprintf(d, sizeof d, "%llu", static_cast<unsigned long long>(count));
      const NevrLogField f[] = {{"stack", a}, {"stack_cap", b}, {"cap_read", c}, {"count", d}, {"readable", ok ? "1" : "0"}};
      Log(NEVR_LOG_WARNING, "memsys: layout pool NOT raised (layout not recognised); the inline fill gives the stock 512", f, 5);
    }
    return false;
  }
  // the whole free stack must be readable before a store goes into it
  (void)Rd<std::uint32_t>(stack, &ok);
  (void)Rd<std::uint32_t>(stack + 4ull * (kCapNew - 1), &ok);
  if (!ok) {
    if (!g_pool_logged.exchange(true))
      Log(NEVR_LOG_WARNING, "memsys: layout pool NOT raised (free stack not readable); the inline fill gives the stock 512");
    return false;
  }
  auto* s = reinterpret_cast<std::uint32_t*>(stack);
  for (std::uint32_t i = 0; i < kCapNew - kCapStock; ++i) s[i] = kCapNew - 1 - i;  // 1023..512, below the inline 511..0
  *reinterpret_cast<std::uint64_t*>(pool + kOffCount) = kCapNew - kCapStock;
  if (!g_pool_logged.exchange(true)) {
    const KV kv[] = {{"slots", kCapNew}, {"pre_pushed", kCapNew - kCapStock}, {"cap_read_back", cap}};
    LogNums(NEVR_LOG_INFO, "memsys: layout pool raised", kv, 3);
  }
  return true;
}

__attribute__((noinline)) std::uint64_t AllocWrapper(void* pool, std::uint32_t cap, void* allocator) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const auto real = reinterpret_cast<AllocFn>(g_alloc_real.load(std::memory_order_acquire));
  g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_alloc_ret.store(ret, std::memory_order_relaxed);
  if (real == nullptr) return 0;
  if (ret != SiteAlloc() || cap != kCapStock || pool == nullptr) return real(pool, cap, allocator);
  const std::uint64_t r = real(pool, kCapNew, allocator);
  if (RaisePool(reinterpret_cast<std::uintptr_t>(pool))) {
    g_pool_raised.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_pool_not_raised.fetch_add(1, std::memory_order_relaxed);
  }
  return r;
}

// android_main's first call. The value of the System heap word the game is about to read is the witness that the plugin was
// in before android_main got there (the heaps module swaps that slot).
void Witness() {
  bool ok = true;
  const auto word = Rd<std::uintptr_t>(g_base + g_pins.got_sys, &ok);
  const auto size = ok ? Rd<std::uint64_t>(word, &ok) : 0;
  char mib[24], expect[24];
  std::snprintf(mib, sizeof mib, "%llu", static_cast<unsigned long long>(size >> 20));
  std::snprintf(expect, sizeof expect, "0x%llx", static_cast<unsigned long long>(g_base + g_pins.got_sys));
  const NevrLogField f[] = {{"system_heap_mib_next_read", ok ? mib : "unreadable"}, {"system_slot", expect},
                            {"plugin_armed_before_android_main", "1"}};
  Log(NEVR_LOG_INFO, "memsys: android_main reached CSysJava::Init", f, 3);
}

__attribute__((noinline)) std::uint64_t InitWrapper(void* app) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const auto real = reinterpret_cast<InitFn>(g_init_real.load(std::memory_order_acquire));
  g_init_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_init_ret.store(ret, std::memory_order_relaxed);
  if (ret == SiteInit() && !g_init_seen.exchange(true)) Witness();
  return real != nullptr ? real(app) : 0;
}

void ReportHook(const char* row, const char* symbol, const InstallResult& r) {
  const NevrLogField f[] = {{"row", row}, {"symbol", symbol}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  Log(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "memsys hook install", f, 4);
}

// False when a pinned word of `row` is not the stock one (one WARNING naming the first).
bool WordsMatch(std::uintptr_t base, int row, const char* name) {
  for (int i = 0; i < kWordCount; ++i) {
    if (kWords[i].row != row) continue;
    bool ok = true;
    const auto have = Rd<std::uint32_t>(base + kWords[i].vaddr, &ok);
    if (!ok || have != kWords[i].word) {
      char at[24], h[16], w[16];
      std::snprintf(at, sizeof at, "0x%llx", static_cast<unsigned long long>(kWords[i].vaddr));
      std::snprintf(h, sizeof h, "%08x", have);
      std::snprintf(w, sizeof w, "%08x", kWords[i].word);
      const NevrLogField f[] = {{"row", name}, {"vaddr", at}, {"have", ok ? h : "unreadable"}, {"want", w}};
      Log(NEVR_LOG_WARNING, "memsys row refused: a pinned code word differs (not the pinned build?)", f, 4);
      return false;
    }
  }
  return true;
}

}  // namespace

MemsysPins DefaultMemsysPins() { return kDefaultPins; }

const MemsysWord* MemsysWords(int* count) {
  if (count != nullptr) *count = kWordCount;
  return kWords;
}

InstallResult InstallMemsys(std::uintptr_t base) {
  InstallResult result;
  if (base == 0) {
    result.reason = "no_base";
    Log(NEVR_LOG_ERROR, "memsys not installed: no game base");
    return result;
  }
  result.status = SwapStatus::kOk;
  if (ModuleSwitchPresent("memsys.off")) {
    result.reason = "memsys_off";
    Log(NEVR_LOG_INFO, "memsys off: memsys.off present, nothing changed");
    return result;
  }
  g_base = base;
  const struct {
    int row;
    const char* id;
    const char* symbol;
    std::uintptr_t slot, real;
    std::uintptr_t wrapper;
    std::atomic<std::uintptr_t>* real_out;
  } hooks[] = {
      {37, "R37", "_ZN10NRadEngine11CSimplePoolINS_20SGRenderTargetLayoutEjE5AllocEjPNS_10CAllocatorE", g_pins.got_alloc,
       g_pins.fn_alloc, reinterpret_cast<std::uintptr_t>(&AllocWrapper), &g_alloc_real},
      {38, "R38", "_ZN10NRadEngine8CSysJava4InitEP11android_app", g_pins.got_init, g_pins.fn_init,
       reinterpret_cast<std::uintptr_t>(&InitWrapper), &g_init_real},
  };
  for (const auto& h : hooks) {
    InstallResult r;
    if (!WordsMatch(base, h.row, h.id)) {
      r.status = SwapStatus::kUnexpectedOriginal;
      r.reason = "pinned_word_mismatch";
    } else {
      r = InstallImportHook(h.symbol, base, h.slot, h.wrapper, h.real_out, h.real);
    }
    ReportHook(h.id, h.symbol, r);
    if (r.status != SwapStatus::kOk && result.status == SwapStatus::kOk) {
      result.status = r.status;
      result.reason = h.id;
    }
  }
  return result;
}

MemsysStats GetMemsysStats() {
  return {g_alloc_calls.load(), g_pool_raised.load(), g_pool_not_raised.load(), g_init_calls.load(),
          g_init_seen.load() ? 1u : 0u};
}

void SetMemsysPinsForTest(const MemsysPins& pins) { g_pins = pins; }
void SetMemsysSitesForTest(std::uintptr_t alloc_ret, std::uintptr_t init_ret) {
  g_site_alloc.store(alloc_ret);
  g_site_init.store(init_ret);
}
void GetMemsysLastReturnsForTest(std::uintptr_t* alloc_ret, std::uintptr_t* init_ret) {
  if (alloc_ret != nullptr) *alloc_ret = g_last_alloc_ret.load();
  if (init_ret != nullptr) *init_ret = g_last_init_ret.load();
}
void ResetMemsysForTest() {
  g_pins = kDefaultPins;
  g_base = 0;
  g_alloc_real.store(0);
  g_init_real.store(0);
  g_site_alloc.store(0);
  g_site_init.store(0);
  g_last_alloc_ret.store(0);
  g_last_init_ret.store(0);
  g_alloc_calls.store(0);
  g_pool_raised.store(0);
  g_pool_not_raised.store(0);
  g_init_calls.store(0);
  g_pool_logged.store(false);
  g_init_seen.store(false);
}

}  // namespace q15t

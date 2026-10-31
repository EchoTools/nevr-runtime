#include "custctx.h"

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "got_swap.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

constexpr std::uint64_t kDrawSize = 0x9718;  // sizeof(SR15NetCustomDrawData): Grow's `mov w9,#0x9718; mul x1,(add+capacity),x9` (disassembly 0x1248970)
constexpr unsigned kMax = 256, kDropLines = 16, kTotalEvery = 50, kTotalLines = 16, kSpares = 4;
constexpr unsigned kRtEvery = 32, kRtLines = 32;
constexpr std::uint64_t kRtMargin = 64;  // free layout slots a load needs left over (one per new mesh entry)
constexpr std::uintptr_t kRtCap = 0x84, kRtFree = 0x30;  // the pool: capacity (u32), free count (u64); disassembly 0x18829c8/0x18829cc

constexpr const char kLoadSym[] =
    "_ZN10NRadEngine8NRadGame28CR15NetCustomizationRenderer12LoadMeshDataERNS0_27SR15NetCustomizationContextERNS_16CComponentHandleINS_8CModelCSEEE";
constexpr const char kDestroySym[] =
    "_ZN10NRadEngine8NRadGame28CR15NetCustomizationRenderer14DestroyContextEPNS0_27SR15NetCustomizationContextE";
constexpr const char kGrowSym[] = "_ZN10NRadEngine8CTableXTINS_8NRadGame21SR15NetCustomDrawDataEE4GrowEv";
constexpr const char kResizeSym[] = "_ZN10NRadEngine9CMemBlock6ResizeEyj";

struct DrawTable {          // CTableXT<SR15NetCustomDrawData> as Grow reads it (CMemBlock first); disassembly 0x1248948 ldp x9,x8,[x0,#0x28]; 0x1248958 ldr x8,[x19,#0x20]
  std::uint8_t* data;       // +0x00
  std::uint64_t size;       // +0x08
  void* allocator;          // +0x10
  std::uint32_t align, flags;  // +0x18
  std::uint64_t step;       // +0x20
  std::uint64_t capacity;   // +0x28
  std::uint64_t count;      // +0x30
};
static_assert(offsetof(DrawTable, step) == 0x20 && offsetof(DrawTable, capacity) == 0x28 && offsetof(DrawTable, count) == 0x30,
              "CTableXT layout");

using LoadFn = std::uint64_t (*)(void* self, void* ctx, void* model);
using DestroyFn = std::uint64_t (*)(void* self, void* ctx);
using GrowFn = void* (*)(DrawTable* t);

CustCtxAddrs g_addrs{};
std::uintptr_t g_base = 0;
CustCtxFns g_f{};
std::atomic<std::uintptr_t> g_load{0}, g_destroy{0}, g_grow{0};
std::atomic<unsigned> g_drops{0}, g_lines{0}, g_totals{0};
std::atomic<unsigned> g_fails{0}, g_fail_lines{0}, g_fail_totals{0}, g_spare_next{0};
std::atomic<unsigned> g_loads{0}, g_rt_lines{0}, g_rt_refused{0};
std::atomic<bool> g_full_logged{false};
void* g_spares[kSpares];  // set at install, before the swap: elements for when the memalign fails too
int (*g_memalign)(void**, std::size_t, std::size_t) = nullptr;
char g_dir_override[256] = "";

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
struct Entry {
  const void* ctx;
  std::uint32_t inflight;
  std::uint8_t live, dying;
};
Entry g_tab[kMax];  // under g_mu; ctx == nullptr = free

int Memalign(void** out, std::size_t align, std::size_t size) {
  return g_memalign != nullptr ? g_memalign(out, align, size) : posix_memalign(out, align, size);
}

bool OffFilePresent(const char* name) {
  char dir[320];
  if (g_dir_override[0] != 0) {
    std::snprintf(dir, sizeof dir, "%s", g_dir_override);
  } else {
    char cmd[256] = "";
    FILE* f = std::fopen("/proc/self/cmdline", "re");
    if (f == nullptr) return false;
    const std::size_t r = std::fread(cmd, 1, sizeof cmd - 1, f);
    std::fclose(f);
    cmd[r] = 0;
    if (char* colon = std::strchr(cmd, ':')) *colon = 0;
    if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160) return false;
    std::snprintf(dir, sizeof dir, "/sdcard/Android/data/%s/files/q15t", cmd);
  }
  char path[640];
  std::snprintf(path, sizeof path, "%s/%s", dir, name);
  return access(path, F_OK) == 0;
}

Entry* Find(const void* ctx) {
  for (Entry& e : g_tab)
    if (e.ctx == ctx) return &e;
  return nullptr;
}

Entry* FindOrAdd(const void* ctx) {
  Entry* free_slot = nullptr;
  for (Entry& e : g_tab) {
    if (e.ctx == ctx) return &e;
    if (free_slot == nullptr && e.ctx == nullptr) free_slot = &e;
  }
  if (free_slot != nullptr) *free_slot = Entry{ctx, 0, 0, 0};
  return free_slot;
}

// An entry nobody needs any more is freed. (No in-flight test: every LoadMeshData entry sets live, so a load in flight has live != 0.)
void Release(Entry* e) {
  if (e != nullptr && e->live == 0 && e->dying == 0) e->ctx = nullptr;
}

void Hex(char* out, std::size_t n, unsigned long long v) { std::snprintf(out, n, "0x%llx", v); }

void Full() {
  if (!g_full_logged.exchange(true, std::memory_order_relaxed)) {
    const KV f[] = {{"max", static_cast<long long>(kMax)}};
    LogNums(NEVR_LOG_WARNING, "custctx more than max contexts tracked: stock behaviour for the overflow (logged once)", f, 1);
  }
}

// The layout pool's free slots (a racy hint: the pool only changes on the render / load threads). Any unreadable word: not low.
bool RtLow(std::uint64_t* used, std::uint32_t* cap) {
  bool ok = true;
  const auto pool = Rd<std::uintptr_t>(g_base + g_addrs.rt_pool, &ok);  // a null or unreadable pool pointer fails the reads below too
  const auto c = Rd<std::uint32_t>(pool + kRtCap, &ok);
  const auto fr = Rd<std::uint64_t>(pool + kRtFree, &ok);
  if (!ok) return false;
  *cap = c;
  *used = c - fr;
  return fr < kRtMargin;
}

__attribute__((noinline)) std::uint64_t LoadWrapper(void* self, void* ctx, void* model) {
  bool tracked = false;
  pthread_mutex_lock(&g_mu);
  if (Entry* e = FindOrAdd(ctx)) {
    e->live = 1;
    e->dying = 0;
    ++e->inflight;
    tracked = true;
  }
  pthread_mutex_unlock(&g_mu);
  if (!tracked) Full();
  std::uint64_t used = 0;
  std::uint32_t cap = 0;
  const bool low = RtLow(&used, &cap);
  const unsigned loads = g_loads.fetch_add(1, std::memory_order_relaxed) + 1;
  if (loads % kRtEvery == 0 && g_rt_lines.fetch_add(1, std::memory_order_relaxed) < kRtLines) {
    const KV f[] = {{"loads", static_cast<long long>(loads)}, {"layouts_used", static_cast<long long>(used)}, {"layouts_capacity", static_cast<long long>(cap)}};
    LogNums(NEVR_LOG_INFO, "custctx cosmetic loads", f, 3);
  }
  // A full pool is a deliberate trap (WriteLog + brk) in CGRenderTargetState::Initialize on this load's new mesh entries: fail the
  // load instead, as LoadMeshData does when a resource is missing (CreateContext then destroys the empty context, returns NULL).
  std::uint64_t r = 1;
  if (low) {
    const unsigned n = g_rt_refused.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kDropLines || n % kTotalEvery == 0) {
      const KV f[] = {{"layouts_used", static_cast<long long>(used)}, {"layouts_capacity", static_cast<long long>(cap)}, {"refused", static_cast<long long>(n)}};
      LogNums(NEVR_LOG_INFO, "custctx cosmetic load refused: render target layouts nearly full, this player's cosmetics are skipped", f, 3);
    }
  } else {
    r = reinterpret_cast<LoadFn>(g_load.load(std::memory_order_acquire))(self, ctx, model);
  }
  if (tracked) {
    pthread_mutex_lock(&g_mu);
    Entry* e = Find(ctx);
    if (e != nullptr && e->inflight != 0) --e->inflight;
    pthread_mutex_unlock(&g_mu);
  }
  return r;
}

void Drop(const void* ctx, const char* why) {
  const unsigned n = g_drops.fetch_add(1, std::memory_order_relaxed) + 1;
  if (g_lines.fetch_add(1, std::memory_order_relaxed) < kDropLines) {
    char c[24], d[16];
    Hex(c, sizeof c, reinterpret_cast<std::uintptr_t>(ctx));
    std::snprintf(d, sizeof d, "%u", n);
    const NevrLogField f[] = {{"ctx", c}, {"why", why}, {"dropped", d}};
    Log(NEVR_LOG_INFO, "custctx dropped destroy", f, 3);
  } else if (n % kTotalEvery == 0 && g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
    const KV f[] = {{"dropped", static_cast<long long>(n)}};
    LogNums(NEVR_LOG_INFO, "custctx destroys dropped so far", f, 1);
  }
}

__attribute__((noinline)) std::uint64_t DestroyWrapper(void* self, void* ctx) {
  const auto real = reinterpret_cast<DestroyFn>(g_destroy.load(std::memory_order_acquire));
  if (ctx == nullptr) return real(self, ctx);  // the stock function returns at once
  const char* why = nullptr;
  bool tracked = false;
  pthread_mutex_lock(&g_mu);
  Entry* e = Find(ctx);
  if (e == nullptr) why = "not live: no context holds this address";
  else if (e->inflight != 0) why = "its LoadMeshData is running";
  else if (e->dying != 0) why = "already being destroyed";
  else if (e->live == 0) why = "not live: already destroyed";
  else {
    e->live = 0;
    e->dying = 1;
    tracked = true;
  }
  pthread_mutex_unlock(&g_mu);
  if (why != nullptr) {
    Drop(ctx, why);
    return 0;
  }
  const std::uint64_t r = real(self, ctx);
  if (tracked) {
    pthread_mutex_lock(&g_mu);
    if (Entry* d = Find(ctx)) {
      d->dying = 0;
      Release(d);
    }
    pthread_mutex_unlock(&g_mu);
  }
  return r;
}

// A table that would be written through NULL: made empty; Grow gets an element that is in no table.
void* Spare(DrawTable* t, const DrawTable& was, std::uint64_t add, std::uint64_t wanted, const char* why) {
  t->data = nullptr;
  t->size = 0;
  t->capacity = 0;
  t->count = 0;
  const unsigned n = g_fails.fetch_add(1, std::memory_order_relaxed) + 1;
  if (g_fail_lines.fetch_add(1, std::memory_order_relaxed) < kDropLines) {
    char e[24], w[24], u[24], nb[16];
    std::snprintf(e, sizeof e, "%llu", static_cast<unsigned long long>(add));
    std::snprintf(w, sizeof w, "%llu", static_cast<unsigned long long>(wanted));
    std::snprintf(u, sizeof u, "%llu", static_cast<unsigned long long>(was.count));
    std::snprintf(nb, sizeof nb, "%u", n);
    const NevrLogField f[] = {{"entries", e}, {"bytes", w}, {"in_use", u}, {"why", why}, {"failure", nb}};
    Log(NEVR_LOG_WARNING, "custctx no memory for cosmetic draw entries: this player's cosmetics are skipped", f, 5);
  } else if (n % kTotalEvery == 0 && g_fail_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
    const KV f[] = {{"failures", static_cast<long long>(n)}};
    LogNums(NEVR_LOG_WARNING, "custctx draw-table failures so far", f, 1);
  }
  // Grow itself builds the element in a one-slot table of ours; the slot is never freed (a resource load may still point into it),
  // the table header only lives for this call (a stack local).
  DrawTable one{};
  void* slot = nullptr;
  if (Memalign(&slot, 64, kDrawSize) != 0) slot = nullptr;
  if (slot == nullptr) slot = g_spares[g_spare_next.fetch_add(1, std::memory_order_relaxed) % kSpares];  // all set at install
  one.data = static_cast<std::uint8_t*>(slot);
  one.size = kDrawSize;
  one.allocator = was.allocator;
  one.step = 1;
  one.capacity = 1;
  return reinterpret_cast<GrowFn>(g_grow.load(std::memory_order_acquire))(&one);
}

__attribute__((noinline)) void* GrowWrapper(DrawTable* t) {
  const auto real = reinterpret_cast<GrowFn>(g_grow.load(std::memory_order_acquire));
  DrawTable was;
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(t), &was, sizeof was)) return real(t);  // doubt: the real function decides
  if (was.count >= was.capacity) {
    const std::uint64_t add = was.step < was.capacity ? was.capacity : was.step;  // as Grow picks it (disassembly 0x1248964/68)
    const std::uint64_t wanted = (was.capacity + add) * kDrawSize;
    if (add == 0) return Spare(t, was, add, wanted, "step and capacity 0");
    g_f.resize(t, wanted, 1);
    bool ok = true;
    const auto data = Rd<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(t) + offsetof(DrawTable, data), &ok);
    if (!ok || data == 0) return Spare(t, was, add, wanted, "Resize left the data NULL");
    t->capacity = was.capacity + add;
  } else if (was.data == nullptr) {
    return Spare(t, was, 0, 0, "room left but the data is NULL: a torn-down table");
  }
  return real(t);  // count < capacity and data set: Grow only adds the element
}

// word at `slot` is `wrapper` -> `real` again; false when it could not be put back
bool Unhook(std::uintptr_t slot_vaddr, std::uintptr_t wrapper, std::uintptr_t real) {
  const SwapResult r = SwapWord(g_base + slot_vaddr, wrapper, real);
  if (r.status != SwapStatus::kOk) {
    char s[24];
    Hex(s, sizeof s, slot_vaddr);
    const NevrLogField f[] = {{"slot_vaddr", s}, {"status", StatusName(r.status)}};
    Log(NEVR_LOG_ERROR, "custctx could not put a slot word back", f, 2);
  }
  return r.status == SwapStatus::kOk;
}

InstallResult Refuse(InstallResult r, const char* which, std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr) {
  char msg[96];
  std::snprintf(msg, sizeof msg, "custctx refused (%s): nothing changed (or restore failed, see status)", which);
  LogSlotRefusal(msg, base, slot_vaddr, real_vaddr, r);
  return r;
}

}  // namespace

bool CustCtxResizeMatches(std::uintptr_t base, const CustCtxAddrs& addrs, const void* resize, CustCtxFns* out) {
  if (resize == nullptr || reinterpret_cast<std::uintptr_t>(resize) != base + addrs.fn_resize) return false;
  out->resize = reinterpret_cast<void (*)(void*, std::uint64_t, std::uint32_t)>(const_cast<void*>(resize));
  return true;
}

bool ResolveCustCtxFns(std::uintptr_t base, const CustCtxAddrs& addrs, CustCtxFns* out) {
  void* h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  void* p = dlsym(h, kResizeSym);
  dlclose(h);
  return CustCtxResizeMatches(base, addrs, p, out);
}

InstallResult InstallCustCtxAt(std::uintptr_t base, const CustCtxAddrs& addrs, const CustCtxFns& fns) {
  InstallResult r;
  if (OffFilePresent("custctx.off")) {
    const NevrLogField f[] = {{"why", "custctx.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "custctx off", f, 1);
    r.reason = "off_file";
    return r;
  }
  if (fns.resize == nullptr) {
    const NevrLogField f[] = {{"why", "CMemBlock::Resize not exported at the pinned address"}};
    Log(NEVR_LOG_ERROR, "custctx refused: nothing changed", f, 1);
    r.reason = "export_missing";
    return r;
  }
  g_f = fns;
  g_addrs = addrs;
  g_base = base;
  for (void*& s : g_spares)
    if (Memalign(&s, 64, kDrawSize) != 0) s = nullptr;
  for (void* s : g_spares) {
    if (s == nullptr) {  // Spare must never hand Grow a NULL slot: without every spare, swap nothing
      for (void*& p : g_spares) {
        std::free(p);
        p = nullptr;
      }
      const NevrLogField f[] = {{"why", "could not preallocate the spare draw entries"}};
      Log(NEVR_LOG_ERROR, "custctx refused: nothing changed", f, 1);
      r.reason = "spare_alloc_failed";
      return r;
    }
  }
  const auto w_load = reinterpret_cast<std::uintptr_t>(&LoadWrapper);
  const auto w_destroy = reinterpret_cast<std::uintptr_t>(&DestroyWrapper);
  const auto w_grow = reinterpret_cast<std::uintptr_t>(&GrowWrapper);
  r = InstallImportHook(kLoadSym, base, addrs.got_load, w_load, &g_load, addrs.fn_load);
  if (r.status != SwapStatus::kOk) {
    if (r.status == SwapStatus::kRestoreFailed) Unhook(addrs.got_load, w_load, base + addrs.fn_load);
    return Refuse(r, "LoadMeshData", base, addrs.got_load, addrs.fn_load);
  }
  InstallResult r2 = InstallImportHook(kDestroySym, base, addrs.got_destroy, w_destroy, &g_destroy, addrs.fn_destroy);
  if (r2.status != SwapStatus::kOk) {  // all or none
    if (r2.status == SwapStatus::kRestoreFailed) Unhook(addrs.got_destroy, w_destroy, base + addrs.fn_destroy);
    Unhook(addrs.got_load, w_load, base + addrs.fn_load);
    return Refuse(r2, "DestroyContext", base, addrs.got_destroy, addrs.fn_destroy);
  }
  InstallResult r3 = InstallImportHook(kGrowSym, base, addrs.got_grow, w_grow, &g_grow, addrs.fn_grow);
  if (r3.status != SwapStatus::kOk) {
    if (r3.status == SwapStatus::kRestoreFailed) Unhook(addrs.got_grow, w_grow, base + addrs.fn_grow);
    Unhook(addrs.got_destroy, w_destroy, base + addrs.fn_destroy);
    Unhook(addrs.got_load, w_load, base + addrs.fn_load);
    return Refuse(r3, "Grow", base, addrs.got_grow, addrs.fn_grow);
  }
  char a[24], b[24], c[24];
  Hex(a, sizeof a, addrs.got_load);
  Hex(b, sizeof b, addrs.got_destroy);
  Hex(c, sizeof c, addrs.got_grow);
  char margin[16];
  std::snprintf(margin, sizeof margin, "%llu", static_cast<unsigned long long>(kRtMargin));
  const NevrLogField f[] = {{"slot_load", a}, {"slot_destroy", b}, {"slot_grow", c}, {"layout_margin", margin}};
  Log(NEVR_LOG_INFO,
      "custctx on: LoadMeshData tracks live contexts and refuses a load on a nearly full layout pool, DestroyContext runs only for a live "
      "context with no load in flight, Grow checks its resize",
      f, 4);
  return r;
}

bool CustCtxWordPinsMatch(std::uintptr_t base) { return CheckWordPins("custctx", base, kCustCtxWordPins, kCustCtxWordPinCount); }

InstallResult InstallCustCtx(std::uintptr_t base) {
  if (!CustCtxWordPinsMatch(base)) {
    InstallResult r;
    r.reason = "word_pin_mismatch";
    return r;
  }
  CustCtxFns fns{};
  if (!ResolveCustCtxFns(base, kCustCtxPinned, &fns)) {
    const NevrLogField f[] = {{"why", "CMemBlock::Resize not exported at the pinned address"}};
    Log(NEVR_LOG_ERROR, "custctx refused: nothing changed", f, 1);
    InstallResult r;
    r.reason = "export_missing";
    return r;
  }
  return InstallCustCtxAt(base, kCustCtxPinned, fns);
}

CustCtxStats GetCustCtxStats() {
  return {g_drops.load(std::memory_order_relaxed), g_fails.load(std::memory_order_relaxed), g_rt_refused.load(std::memory_order_relaxed)};
}

void SetCustCtxDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void SetCustCtxMemalignForTest(int (*memalign)(void**, std::size_t, std::size_t)) { g_memalign = memalign; }
void ResetCustCtxForTest() {
  pthread_mutex_lock(&g_mu);
  std::memset(g_tab, 0, sizeof g_tab);
  pthread_mutex_unlock(&g_mu);
  g_drops = g_lines = g_totals = 0;
  g_fails = g_fail_lines = g_fail_totals = g_spare_next = 0;
  g_loads = g_rt_lines = g_rt_refused = 0;
  g_full_logged = false;
  g_load = g_destroy = g_grow = 0;
  g_f = CustCtxFns{};
  g_addrs = CustCtxAddrs{};
  g_base = 0;
  g_memalign = nullptr;
}

}  // namespace q15t

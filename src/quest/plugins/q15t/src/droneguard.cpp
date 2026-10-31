#include "droneguard.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// CR15NetFollowPlayerCS layout (the source build's droneguard:46-60; disassembly 0x21497bc ldr x20,[x19,#0x140]; 0x21497cc ldr w1,[x19,#0x190];
// 0x214980c ldrh w8,[x19,#0x190]): the followed system, the player handle at +0x148 and its cached system / index / flags.
constexpr std::uintptr_t kTarget = 0x140, kHandle = 0x148, kHandleFlags = 0x16c, kHandleSys = 0x188, kHandleIdx = 0x190;
constexpr std::size_t kSpan = kHandleIdx + 4 - kTarget;  // one read covers every field above
constexpr unsigned kTotalEvery = 600, kTotalLines = 8, kStateLines = 16;
constexpr const char kSymbol[] = "_ZN10NRadEngine8NRadGame21CR15NetFollowPlayerCS6FollowENS_18SCompactPoolHandleE";
constexpr const char kRefreshSym[] = "_ZNK10NRadEngine16CComponentHandleINS_8NRadGame15CR15PlayerNavCSEE12RefreshCacheEv";
constexpr const char kValidSym[] = "_ZNK10NRadEngine8NRadGame15CR15PlayerNavCS16IsComponentValidENS_18SCompactPoolHandleE";

using FollowFn = void (*)(void* cs, std::uint64_t handle);

struct Snap {  // the span [cs+0x140, cs+0x194)
  std::uintptr_t target;
  std::uintptr_t sys;
  std::uint32_t idx;
  std::uint8_t flags;
};

std::atomic<std::uintptr_t> g_real{0};
DroneGuardFns g_f{};  // set before the swap, read-only after
std::atomic<bool> g_idx_on{false};
std::atomic<unsigned> g_skips{0}, g_totals{0}, g_idx_skips{0}, g_idx_totals{0}, g_state_lines{0};
std::atomic<bool> g_skipping{false}, g_idx_skipping{false};
std::atomic<std::uint64_t> g_state{~0ull};  // no real key has bits above 41 set

bool ReadSnap(std::uintptr_t cs, Snap* out) {
  std::uint8_t buf[kSpan];
  if (!SafeRead(cs + kTarget, buf, sizeof buf)) return false;
  std::memcpy(&out->target, buf, sizeof out->target);
  std::memcpy(&out->sys, buf + (kHandleSys - kTarget), sizeof out->sys);
  std::memcpy(&out->idx, buf + (kHandleIdx - kTarget), sizeof out->idx);
  std::memcpy(&out->flags, buf + (kHandleFlags - kTarget), sizeof out->flags);
  return true;
}

void Hex(char* out, std::size_t n, unsigned long long v) { std::snprintf(out, n, "0x%llx", v); }

// One line on the first call and on every change of (raw index word, flag byte, target null, handle system null), at most
// kStateLines. Read-only: the values are the previous frame's cache (Follow refreshes it itself).
void StateLine(std::uintptr_t cs, const Snap& s) {
  const std::uint64_t key = s.idx | (static_cast<std::uint64_t>(s.flags) << 32) | (static_cast<std::uint64_t>(s.target != 0) << 40) |
                            (static_cast<std::uint64_t>(s.sys != 0) << 41);
  if (g_state.exchange(key, std::memory_order_relaxed) == key) return;
  if (g_state_lines.fetch_add(1, std::memory_order_relaxed) >= kStateLines) return;
  char c[24], raw[16], fl[8];
  Hex(c, sizeof c, cs);
  std::snprintf(raw, sizeof raw, "%08x", s.idx);
  std::snprintf(fl, sizeof fl, "%02x", s.flags);
  const NevrLogField f[] = {{"target", s.target != 0 ? "set" : "0"},
                            {"handle_sys", s.sys != 0 ? "set" : "0"},
                            {"idx_unresolved", (s.idx & 0xffff) == 0xffff ? "yes" : "no"},
                            {"raw190", raw},
                            {"flags16c", fl},
                            {"cs", c}};
  Log(NEVR_LOG_INFO, "droneguard state (previous frame's cache)", f, 6);
}

// nullptr = run Follow; else the skip reason
const char* IdxSkip(std::uintptr_t cs) {
  g_f.refresh(reinterpret_cast<void*>(cs + kHandle));
  Snap s{};
  if (!ReadSnap(cs, &s)) return nullptr;  // doubt: Follow runs
  if ((s.idx & 0xffff) == 0xffff) return "idx ffff";
  if (s.sys == 0) return "handle-sys 0";
  if (!g_f.is_valid(reinterpret_cast<const void*>(s.target), s.idx)) return "refused but cached";
  return nullptr;
}

__attribute__((noinline)) void FollowWrapper(void* self, std::uint64_t handle) {
  const auto real = reinterpret_cast<FollowFn>(g_real.load(std::memory_order_acquire));
  if (real == nullptr) return;
  const auto cs = reinterpret_cast<std::uintptr_t>(self);
  Snap s{};
  if (!ReadSnap(cs, &s)) {  // doubt: Follow decides (it would fault on a bad pointer with or without us)
    real(self, handle);
    return;
  }
  StateLine(cs, s);
  if (s.target == 0) {
    const unsigned n = g_skips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_skipping.exchange(true, std::memory_order_relaxed)) {
      char c[24];
      Hex(c, sizeof c, cs);
      const NevrLogField f[] = {{"cs", c}};
      Log(NEVR_LOG_INFO, "droneguard first skip: follow target null", f, 1);
    } else if (n % kTotalEvery == 0 && g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
      const KV f[] = {{"skips", static_cast<long long>(n)}};
      LogNums(NEVR_LOG_INFO, "droneguard skips so far", f, 1);
    }
    return;
  }
  if (g_skipping.exchange(false, std::memory_order_relaxed)) {
    const KV f[] = {{"skips", static_cast<long long>(g_skips.load(std::memory_order_relaxed))}};
    LogNums(NEVR_LOG_INFO, "droneguard target set after skips", f, 1);
  }
  if (g_idx_on.load(std::memory_order_relaxed)) {
    if (const char* why = IdxSkip(cs)) {
      const unsigned n = g_idx_skips.fetch_add(1, std::memory_order_relaxed) + 1;
      if (!g_idx_skipping.exchange(true, std::memory_order_relaxed)) {
        char c[24];
        Hex(c, sizeof c, cs);
        const NevrLogField f[] = {{"why", why}, {"cs", c}};
        Log(NEVR_LOG_INFO, "droneguard first idx skip", f, 2);
      } else if (n % kTotalEvery == 0 && g_idx_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
        const KV f[] = {{"idx_skips", static_cast<long long>(n)}};
        LogNums(NEVR_LOG_INFO, "droneguard idx skips so far", f, 1);
      }
      return;
    }
    if (g_idx_skipping.exchange(false, std::memory_order_relaxed)) {
      const KV f[] = {{"idx_skips", static_cast<long long>(g_idx_skips.load(std::memory_order_relaxed))}};
      LogNums(NEVR_LOG_INFO, "droneguard player resolved after idx skips", f, 1);
    }
  }
  real(self, handle);
}

}  // namespace

bool DroneGuardExportsMatch(std::uintptr_t base, const void* refresh, const void* valid, DroneGuardFns* out) {
  if (reinterpret_cast<std::uintptr_t>(refresh) != base + kDroneRefreshCache || reinterpret_cast<std::uintptr_t>(valid) != base + kDroneIsValid)
    return false;
  out->refresh = reinterpret_cast<void (*)(void*)>(const_cast<void*>(refresh));
  out->is_valid = reinterpret_cast<bool (*)(const void*, std::uint64_t)>(const_cast<void*>(valid));
  return true;
}

bool ResolveDroneGuardFns(std::uintptr_t base, DroneGuardFns* out) {
  if (!DroneGuardIdxWordPinsMatch(base)) return false;  // stage 1's own pins first (ERROR naming the pin; the null half stays)
  void* h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  void* refresh = dlsym(h, kRefreshSym);
  void* valid = dlsym(h, kValidSym);
  dlclose(h);
  return DroneGuardExportsMatch(base, refresh, valid, out);
}

InstallResult InstallDroneGuardAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr, bool idx_stage,
                                  const DroneGuardFns* fns) {
  bool idx = idx_stage;
  if (idx && (fns == nullptr || fns->refresh == nullptr || fns->is_valid == nullptr)) {
    const NevrLogField f[] = {{"why", "RefreshCache / IsComponentValid not exported at the pinned addresses"}};
    Log(NEVR_LOG_ERROR, "droneguard idx guard off: null half only", f, 1);
    idx = false;
  }
  if (idx) g_f = *fns;
  g_idx_on.store(idx, std::memory_order_relaxed);  // before the swap: the wrapper never sees a half-set stage
  const InstallResult r =
      InstallImportHook(kSymbol, base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&FollowWrapper), &g_real, real_vaddr);
  char slot[24], real[24];
  Hex(slot, sizeof slot, slot_vaddr);
  Hex(real, sizeof real, real_vaddr);
  const NevrLogField f[] = {{"slot_vaddr", slot}, {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  if (r.status != SwapStatus::kOk) {
    LogSlotRefusal("droneguard refused: nothing changed (or restore failed, see status)", base, slot_vaddr, real_vaddr, r);
    return r;
  }
  Log(NEVR_LOG_INFO, "droneguard on (TEST ONLY): CR15NetFollowPlayerCS::Follow skips while the target at +0x140 is null", f, 4);
  if (idx) {
    char a[24], b[24];
    Hex(a, sizeof a, kDroneRefreshCache);
    Hex(b, sizeof b, kDroneIsValid);
    const NevrLogField g[] = {{"refresh_cache", a}, {"is_component_valid", b}};
    Log(NEVR_LOG_INFO, "droneguard idx guard on (TEST ONLY): RefreshCache, then skip on idx ffff / handle-sys 0 / IsComponentValid refusing", g, 2);
  }
  return r;
}

bool DroneGuardWordPinsMatch(std::uintptr_t base) { return CheckWordPins("droneguard", base, kDroneGuardWordPins, kDroneGuardWordPinCount); }

bool DroneGuardIdxWordPinsMatch(std::uintptr_t base) {
  return CheckWordPins("droneguard idx guard", base, kDroneGuardIdxWordPins, kDroneGuardIdxWordPinCount, "null half only");
}

InstallResult InstallDroneGuard(std::uintptr_t base) {
  if (!DroneGuardWordPinsMatch(base)) {
    InstallResult r;
    r.reason = "word_pin_mismatch";
    return r;
  }
  DroneGuardFns fns{};
  const bool resolved = Q15T_DRONEGUARD_IDX && ResolveDroneGuardFns(base, &fns);
  return InstallDroneGuardAt(base, kDroneGuardSlot, kDroneGuardReal, Q15T_DRONEGUARD_IDX != 0, resolved ? &fns : nullptr);
}

DroneGuardStats GetDroneGuardStats() { return {g_skips.load(std::memory_order_relaxed), g_idx_skips.load(std::memory_order_relaxed)}; }

void ResetDroneGuardForTest() {
  g_real.store(0);
  g_f = DroneGuardFns{};
  g_idx_on.store(false);
  g_skips = g_totals = g_idx_skips = g_idx_totals = g_state_lines = 0;
  g_skipping = g_idx_skipping = false;
  g_state.store(~0ull);
}

}  // namespace q15t

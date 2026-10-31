#include "levelname.h"

#include <atomic>
#include <cstdio>

#include "got_swap.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

constexpr std::uint64_t kGlobalMp = 0x3F9915D3001DC28Eull;  // the source build's patch table GLOBAL_MP
constexpr std::uintptr_t kFnVaddr = 0x1202ecc;              // CR15Game::LoadLevelName (disassembly 0x1302ecc)
constexpr std::uintptr_t kPlainVaddr = 0xf133d0;            // PLT stub -> CncaGame::LoadLevelName (disassembly 0x10133d0)
constexpr std::uintptr_t kFlagOffset = 0x7ad8;              // CR15Game+0x7ad8: bit 31 = lowspec
constexpr std::uint32_t kLowspecMask = 0x80000000u;
// Vtable words holding the function (R_AARCH64_ABS64 in .data.rel.ro): CR15Game, CR15NetDedicatedServer, CR15NetLanServer,
// CR15NetServer; each is vtable symbol + 0x270 = address point + 0x260.
constexpr int kSlotCount = 4;
constexpr std::uintptr_t kSlots[kSlotCount] = {0x362ef90, 0x3630e58, 0x3631278, 0x36316f8};
constexpr std::uint64_t kLogFirstKept = 16;

using Fn = std::uint64_t (*)(void* game, std::uint64_t name);

std::atomic<std::uintptr_t> g_real{0}, g_plain{0};
std::atomic<std::uint64_t> g_kept{0};
std::atomic<bool> g_installed{false};

void LogKept(std::uint64_t name, std::uint64_t n) {
  char nb[24], cb[24];
  std::snprintf(nb, sizeof nb, "0x%llx", static_cast<unsigned long long>(name));
  std::snprintf(cb, sizeof cb, "%llu", static_cast<unsigned long long>(n));
  const NevrLogField f[] = {{"name", nb}, {"n", cb}, {"lowspec", "1"}, {"answer", "plain name"}};
  Log(NEVR_LOG_INFO, "levelname global_mp kept its plain name", f, 4);
}

// this = x0 (the CR15Game), name = x1 (CSymbol64); the result (x0) is the name to load. Neither callee reads x2...
__attribute__((noinline)) std::uint64_t Wrapper(void* game, std::uint64_t name) {
  const auto real = reinterpret_cast<Fn>(g_real.load(std::memory_order_acquire));
  if (name == kGlobalMp) {
    bool ok = true;
    const auto flags = Rd<std::uint32_t>(reinterpret_cast<std::uintptr_t>(game) + kFlagOffset, &ok);
    if (ok && (flags & kLowspecMask) != 0) {
      const auto plain = reinterpret_cast<Fn>(g_plain.load(std::memory_order_acquire));
      const std::uint64_t n = g_kept.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= kLogFirstKept) LogKept(name, n);
      return plain(game, name);
    }
  }
  return real(game, name);
}

void LogRefusal(std::uint32_t level, const char* message, int index, std::uintptr_t slot_vaddr, const char* why,
                std::uintptr_t found, std::uintptr_t expected, int rolled_back, int swapped) {
  char ib[16], sb[24], fb[24], eb[24], rb[16], wb[16];
  std::snprintf(ib, sizeof ib, "%d", index);
  std::snprintf(sb, sizeof sb, "0x%llx", static_cast<unsigned long long>(slot_vaddr));
  std::snprintf(fb, sizeof fb, "0x%llx", static_cast<unsigned long long>(found));
  std::snprintf(eb, sizeof eb, "0x%llx", static_cast<unsigned long long>(expected));
  std::snprintf(rb, sizeof rb, "%d", rolled_back);
  std::snprintf(wb, sizeof wb, "%d", swapped);
  const NevrLogField f[] = {{"slot_index", ib}, {"slot_vaddr", sb}, {"why", why},       {"found", fb},
                            {"expected", eb},   {"swapped_before", wb}, {"rolled_back", rb}};
  Log(level, message, f, 7);
}

}  // namespace

InstallResult InstallLevelName(std::uintptr_t base) {
  InstallResult out;
  if (base == 0) {
    out.reason = "no_base";
    const NevrLogField f[] = {{"why", "no_base"}};
    Log(NEVR_LOG_ERROR, "levelname refused", f, 1);
    return out;
  }
  if (g_installed.load(std::memory_order_acquire)) {
    out.status = SwapStatus::kUnexpectedOriginal;
    out.reason = "already_installed";
    const NevrLogField f[] = {{"why", "already_installed"}};
    Log(NEVR_LOG_WARNING, "levelname refused", f, 1);
    return out;
  }
  const std::uintptr_t expected = base + kFnVaddr;
  g_real.store(expected, std::memory_order_release);
  g_plain.store(base + kPlainVaddr, std::memory_order_release);
  const std::uintptr_t wrapper = reinterpret_cast<std::uintptr_t>(&Wrapper);
  bool swapped[kSlotCount] = {false, false, false, false};
  int n_swapped = 0, failed = -1;
  SwapResult bad;
  for (int i = 0; i < kSlotCount; ++i) {
    const SwapResult r = SwapWord(base + kSlots[i], expected, wrapper);
    if (r.status == SwapStatus::kOk || r.status == SwapStatus::kRestoreFailed) {
      swapped[i] = true;
      ++n_swapped;
    }
    if (r.status != SwapStatus::kOk) {
      failed = i;
      bad = r;
      break;
    }
  }
  if (failed >= 0) {
    int rolled_back = 0;
    for (int i = 0; i < kSlotCount; ++i)
      if (swapped[i] && SwapWord(base + kSlots[i], wrapper, expected).status == SwapStatus::kOk) ++rolled_back;
    LogRefusal(NEVR_LOG_ERROR, "levelname refused", failed, kSlots[failed], StatusName(bad.status), bad.original, expected, rolled_back,
               n_swapped);
    out.status = bad.status;
    out.reason = "slot_refused";
    return out;
  }
  g_installed.store(true, std::memory_order_release);
  char rb[24], pb[24], cb[8];
  std::snprintf(rb, sizeof rb, "0x%llx", static_cast<unsigned long long>(expected));
  std::snprintf(pb, sizeof pb, "0x%llx", static_cast<unsigned long long>(base + kPlainVaddr));
  std::snprintf(cb, sizeof cb, "%d", kSlotCount);
  const NevrLogField f[] = {{"slots", cb}, {"real", rb}, {"plain", pb}, {"global_mp", "0x3f9915d3001dc28e"}};
  Log(NEVR_LOG_INFO, "levelname installed", f, 4);
  out.status = SwapStatus::kOk;
  return out;
}

std::uint64_t LevelNameKeptPlainCount() { return g_kept.load(std::memory_order_relaxed); }

std::uintptr_t LevelNameWrapperForTest() { return reinterpret_cast<std::uintptr_t>(&Wrapper); }
void LevelNameTargetsForTest(std::uintptr_t* real, std::uintptr_t* plain) {
  *real = g_real.load();
  *plain = g_plain.load();
}
void SetLevelNameTargetsForTest(std::uintptr_t real, std::uintptr_t plain) {
  g_real.store(real);
  g_plain.store(plain);
}
void ResetLevelNameForTest() {
  g_real.store(0);
  g_plain.store(0);
  g_kept.store(0);
  g_installed.store(false);
}

}  // namespace q15t

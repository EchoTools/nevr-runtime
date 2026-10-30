#include "wallfire.h"

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bvh.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

constexpr std::uint64_t kLogLines = 16;  // blocked shots logged one by one; after that at powers of two

using QueryFn = bool (*)(std::uintptr_t, std::uintptr_t, std::uint64_t);
using OccludedFn = bool (*)(std::uintptr_t, const float*, const float*);

std::atomic<std::uintptr_t> g_real{0};
WallfirePins g_pins{};
std::uintptr_t g_base = 0;
std::atomic<std::uintptr_t> g_last_ret{0};
std::atomic<bool> g_warned_game{false};
std::atomic<std::uint64_t> g_site{0}, g_checked{0}, g_blocked{0}, g_nan{0}, g_game{0};
char g_dir_override[256] = "";

bool OptionFile(const char* name) {
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

// CncaGameSpace -> CncaGame -> vtable -> [+0x278] is the pinned CncaGame::StaticRaycastOccluded; false (the stock answer) otherwise.
bool GameIsPinned(std::uintptr_t gs) {
  std::uintptr_t game = 0, vptr = 0, fn = 0;
  // (a null game or vtable pointer fails the next SafeRead)
  if (!SafeRead(gs + kGsGame, &game, sizeof game)) return false;
  if (!SafeRead(game, &vptr, sizeof vptr)) return false;
  return SafeRead(vptr + kGameVtblSlot, &fn, sizeof fn) && fn == g_base + g_pins.game_occluded;
}

void LogBlocked(std::uint64_t n, const float* s) {
  char cnt[24], chk[24], seg[160];
  std::snprintf(cnt, sizeof cnt, "%llu", static_cast<unsigned long long>(n));
  std::snprintf(chk, sizeof chk, "%llu", static_cast<unsigned long long>(g_checked.load(std::memory_order_relaxed)));
  std::snprintf(seg, sizeof seg, "%.2f %.2f %.2f -> %.2f %.2f %.2f", s[0], s[1], s[2], s[3], s[4], s[5]);
  const NevrLogField f[] = {{"blocked", cnt}, {"checked", chk}, {"segment", seg}};
  Log(NEVR_LOG_INFO, "wallfire blocked a shot", f, 3);
}

__attribute__((noinline)) bool QueryWrapper(std::uintptr_t gs, std::uintptr_t req, std::uint64_t id) {
  const std::uintptr_t ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  g_last_ret.store(ret - g_base, std::memory_order_relaxed);
  const bool r = reinterpret_cast<QueryFn>(g_real.load(std::memory_order_acquire))(gs, req, id);
  if (ret != g_base + g_pins.site || r || gs == 0) return r;  // another caller, or the stock query already found something
  g_site.fetch_add(1, std::memory_order_relaxed);
  float seg[kReqSegFloats];
  bool finite = SafeRead(req, seg, sizeof seg);
  for (unsigned k = 0; finite && k < kReqSegFloats; ++k) finite = std::isfinite(seg[k]);
  if (!finite) {
    g_nan.fetch_add(1, std::memory_order_relaxed);
    return r;
  }
  if (!GameIsPinned(gs)) {
    g_game.fetch_add(1, std::memory_order_relaxed);
    if (!g_warned_game.exchange(true)) {
      const NevrLogField f[] = {{"why", "CncaGame's virtual +0x278 is not the pinned function, or the chain is unreadable: the stock answer is used"}};
      Log(NEVR_LOG_WARNING, "wallfire skipped a shot check", f, 1);
    }
    return r;
  }
  g_checked.fetch_add(1, std::memory_order_relaxed);
  if (!reinterpret_cast<OccludedFn>(g_base + g_pins.occluded_fn)(gs, seg, seg + 3)) return r;
  const std::uint64_t n = g_blocked.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= kLogLines || (n & (n - 1)) == 0) LogBlocked(n, seg);
  return true;
}

void Hex(char* out, std::size_t n, std::uint64_t v) { std::snprintf(out, n, "0x%llx", static_cast<unsigned long long>(v)); }

InstallResult Refuse(std::uint32_t level, const char* reason, const char* detail, SwapStatus status) {
  InstallResult r;
  r.status = status;
  r.reason = reason;
  const NevrLogField f[] = {{"reason", reason}, {"detail", detail}, {"status", StatusName(status)}};
  Log(level, "wallfire refused: nothing changed", f, 3);
  return r;
}

}  // namespace

InstallResult InstallWallfireAt(std::uintptr_t base, const WallfirePins& pins) {
  if (OptionFile("wallfire.off")) {
    const NevrLogField f[] = {{"why", "wallfire.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "wallfire off", f, 1);
    InstallResult r;
    r.reason = "off_file";
    return r;
  }
  if (base == 0) return Refuse(NEVR_LOG_ERROR, "no_base", "", SwapStatus::kNullSlot);
  if (!BvhTreeInstalled()) return Refuse(NEVR_LOG_WARNING, "bvh_not_installed", "the static BVH is not answering CBVHTree::Occluded", SwapStatus::kNullSlot);
  std::uint32_t word = 0;
  if (!SafeRead(base + pins.site, &word, sizeof word)) return Refuse(NEVR_LOG_ERROR, "site_unreadable", "FireGunLocal return site", SwapStatus::kUnmapped);
  if (word != pins.site_word) {
    char have[24];
    Hex(have, sizeof have, word);
    return Refuse(NEVR_LOG_ERROR, "site_word", have, SwapStatus::kUnexpectedOriginal);
  }
  std::uintptr_t slot_word = 0;
  if (!SafeRead(base + pins.occluded_slot, &slot_word, sizeof slot_word)) return Refuse(NEVR_LOG_ERROR, "occluded_unreadable", "", SwapStatus::kUnmapped);
  if (slot_word != base + pins.occluded_fn) return Refuse(NEVR_LOG_ERROR, "occluded_slot", "StaticRaycastOccluded slot", SwapStatus::kUnexpectedOriginal);

  g_base = base;
  g_pins = pins;
  const InstallResult r = InstallImportHook("_ZNK10NRadEngine13CncaGameSpace14QueryCollisionERNS_24SPhQueryCollisionRequestENS_5CPhIdE", base,
                                            pins.query_slot, reinterpret_cast<std::uintptr_t>(&QueryWrapper), &g_real, pins.query_fn);
  char slot[24], real[24];
  Hex(slot, sizeof slot, pins.query_slot);
  Hex(real, sizeof real, pins.query_fn);
  const NevrLogField f[] = {{"hook", "CncaGameSpace::QueryCollision"}, {"slot_vaddr", slot}, {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  if (r.status != SwapStatus::kOk) {
    Log(NEVR_LOG_ERROR, "wallfire hook refused", f, 5);
    return r;
  }
  Log(NEVR_LOG_INFO, "wallfire on: FireGunLocal's wall query also asks the static level for the player to muzzle segment", f, 5);
  return r;
}

InstallResult InstallWallfire(std::uintptr_t base) { return InstallWallfireAt(base, kWallfirePinned); }

WallfireCounters WallfireGetCounters() {
  return {g_site.load(), g_checked.load(), g_blocked.load(), g_nan.load(), g_game.load()};
}

void SetWallfireDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void ResetWallfireForTest() {
  g_real.store(0);
  g_warned_game.store(false);
  g_site = g_checked = g_blocked = g_nan = g_game = 0;
  g_last_ret.store(0);
}
std::uintptr_t WallfireLastRetForTest() { return g_last_ret.load(); }

}  // namespace q15t

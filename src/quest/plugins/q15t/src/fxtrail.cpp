#include "fxtrail.h"

#include <unistd.h>

#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>

#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// CParticleEffectCS layout as EffectTime reads it (disassembly 0x16647e4..: ldr x8,[x0,#0xc8]; ldrh w8,[x8,(h&0xffff)<<2];
// ldr x9,[x0,#0xf8]; madd x8,x8,0x18,x9; ldr x8,[x8,#0x10]; cbz). Source: the source build's fxtrail:48.
constexpr std::uintptr_t kCsIndex = 0xc8, kCsData = 0xf8, kDataStride = 0x18, kDataInst = 0x10;
constexpr const char kSymbol[] = "_ZNK10NRadEngine17CParticleEffectCS10EffectTimeENS_18SCompactPoolHandleE";

using EffectTimeFn = float (*)(const void*, std::uint64_t);
std::atomic<std::uintptr_t> g_real{0};
std::atomic<std::uint64_t> g_ended{0};
char g_dir_override[256] = "";

// Same data directory as weapon_arm.cpp: /sdcard/Android/data/<pkg>/files/q15t (not created here).
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

__attribute__((noinline)) float EffectTimeWrapper(const void* cs, std::uint64_t h) {
  const auto real = reinterpret_cast<EffectTimeFn>(g_real.load(std::memory_order_acquire));
  if (real == nullptr) return 0.0f;
  const float t = real(cs, h);
  const auto c = reinterpret_cast<std::uintptr_t>(cs);
  bool ok = true;
  const auto index = Rd<std::uintptr_t>(c + kCsIndex, &ok);
  const auto data = Rd<std::uintptr_t>(c + kCsData, &ok);
  const auto slot = Rd<std::uint16_t>(index + (h & 0xffff) * 4, &ok);  // a failed read leaves ok false for the rest
  const auto inst = Rd<std::uintptr_t>(data + static_cast<std::uintptr_t>(slot) * kDataStride + kDataInst, &ok);
  if (!ok || inst != 0) return t;
  const std::uint64_t n = g_ended.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((n & (n - 1)) == 0) {  // n = 1, 2, 4, 8, ...: the call path carries counters only
    const KV f[] = {{"ended", static_cast<long long>(n)}};
    LogNums(NEVR_LOG_INFO, "fxtrail null-effect trail ended", f, 1);
  }
  return FLT_MAX;
}

}  // namespace

InstallResult InstallFxTrailAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr) {
  if (OffFilePresent("fxtrail.off")) {
    const NevrLogField f[] = {{"why", "fxtrail.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "fxtrail off", f, 1);
    InstallResult r;
    r.reason = "off_file";
    return r;
  }
  const InstallResult r = InstallImportHook(kSymbol, base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&EffectTimeWrapper), &g_real,
                                            real_vaddr);
  char slot[24], real[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(slot_vaddr));
  std::snprintf(real, sizeof real, "0x%llx", static_cast<unsigned long long>(real_vaddr));
  const NevrLogField f[] = {{"slot_vaddr", slot}, {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  if (r.status == SwapStatus::kOk)
    Log(NEVR_LOG_INFO, "fxtrail on: CParticleEffectCS::EffectTime ends a trail whose effect has no instance", f, 4);
  else
    LogSlotRefusal("fxtrail refused: nothing changed (or restore failed, see status)", base, slot_vaddr, real_vaddr, r);
  return r;
}

bool FxTrailWordPinsMatch(std::uintptr_t base) { return CheckWordPins("fxtrail", base, kFxTrailWordPins, kFxTrailWordPinCount); }

InstallResult InstallFxTrail(std::uintptr_t base) {
  if (!FxTrailWordPinsMatch(base)) {
    InstallResult r;
    r.reason = "word_pin_mismatch";
    return r;
  }
  return InstallFxTrailAt(base, kFxTrailSlot, kFxTrailReal);
}

std::uint64_t FxTrailEnded() { return g_ended.load(std::memory_order_relaxed); }

void SetFxTrailDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void ResetFxTrailForTest() {
  g_ended.store(0);
  g_real.store(0);
}

}  // namespace q15t

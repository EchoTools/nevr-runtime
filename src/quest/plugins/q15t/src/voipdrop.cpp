#include "voipdrop.h"

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// CR15NetVoipReceiverCS layout (the source build's voipdrop:51): the voice hash table at +0x168; its free head (table+0x50), page size
// (table+0x64) and allocator block (table+0x18). disassembly: AllocPage 0x23164d8 add x22,x0,#0x18; 0x2316504 ldr w1,[x19,#0x64];
// 0x2316508 ldr x8,[x8,#0x10] (alloc); 0x23165a8 str w9,[x19,#0x50] (free head); ReceiveVoip 0x21c3698 add x0,x21,#0x168.
constexpr std::uintptr_t kTable = 0x168, kFreeHead = kTable + 0x50, kPageBytes = kTable + 0x64, kPageBlock = kTable + 0x18;
constexpr std::uintptr_t kVtAlloc = 0x10, kVtFree = 0x38;  // CAllocator vtable: the request AllocPage makes, and DestroyContext's free
constexpr unsigned kDropLines = 16, kTotalEvery = 50, kTotalLines = 16;
constexpr const char kSymbol[] =
    "_ZN10NRadEngine8NRadGame21CR15NetVoipReceiverCS11ReceiveVoipENS_18SCompactPoolHandleERKNS0_20SR15NetUserVoipEventEPKvj";

using RecvFn = void (*)(void* self, std::uint64_t handle, const void* event, const void* data, std::uint32_t bytes);
using AllocFn = void* (*)(void* allocator, std::uint32_t bytes);
using FreeFn = void (*)(void* allocator, void* p);

std::atomic<std::uintptr_t> g_real{0};
VoipDropFns g_f{};  // set before the swap, read-only after
std::atomic<unsigned> g_drops{0}, g_lines{0}, g_totals{0};
char g_dir_override[256] = "";

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

// AllocPage's own request, given straight back. False = that allocator has no block of this size now. True also on any
// doubt (an unreadable vtable): the real function then decides.
bool PageAvailable(std::uintptr_t cs, std::uint32_t bytes) {
  void* const was = g_f.current_ptr();
  g_f.set_current(g_f.block_allocator(reinterpret_cast<const void*>(cs + kPageBlock)));
  void* const a = g_f.current();
  bool available = false;
  if (a != nullptr) {
    bool ok = true;
    const auto vt = Rd<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(a), &ok);
    const auto alloc = Rd<std::uintptr_t>(vt + kVtAlloc, &ok);
    const auto release = Rd<std::uintptr_t>(vt + kVtFree, &ok);
    if (!ok || alloc == 0 || release == 0) {
      available = true;
    } else {
      void* p = reinterpret_cast<AllocFn>(alloc)(a, bytes);
      if (p != nullptr) reinterpret_cast<FreeFn>(release)(a, p);
      available = p != nullptr;
    }
  }
  g_f.set_current(was);
  return available;
}

__attribute__((noinline)) void RecvWrapper(void* self, std::uint64_t handle, const void* event, const void* data, std::uint32_t bytes) {
  const auto real = reinterpret_cast<RecvFn>(g_real.load(std::memory_order_acquire));
  if (real == nullptr) return;
  const auto cs = reinterpret_cast<std::uintptr_t>(self);  // null or unreadable: the reads fail and the real function runs
  bool ok = true;
  const auto head = Rd<std::int32_t>(cs + kFreeHead, &ok);
  if (ok && head == -1) {  // the next packet needs a fresh page
    const auto page = Rd<std::uint32_t>(cs + kPageBytes, &ok);
    if (ok && !PageAvailable(cs, page)) {
      const unsigned n = g_drops.fetch_add(1, std::memory_order_relaxed) + 1;
      if (g_lines.fetch_add(1, std::memory_order_relaxed) < kDropLines) {
        const KV f[] = {{"page_bytes", static_cast<long long>(page)}, {"dropped", static_cast<long long>(n)}};
        LogNums(NEVR_LOG_INFO, "voipdrop no memory for a voice table page: packet dropped", f, 2);
      } else if (n % kTotalEvery == 0 && g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
        const KV f[] = {{"dropped", static_cast<long long>(n)}};
        LogNums(NEVR_LOG_INFO, "voipdrop voice packets dropped so far", f, 1);
      }
      return;
    }
  }
  real(self, handle, event, data, bytes);
}

}  // namespace

bool ResolveVoipDropFns(VoipDropFns* out) {
  void* h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  VoipDropFns f{};
  f.block_allocator = reinterpret_cast<void* (*)(const void*)>(dlsym(h, "_ZNK10NRadEngine9CMemBlock9AllocatorEv"));
  f.current_ptr = reinterpret_cast<void* (*)()>(dlsym(h, "_ZN10NRadEngine14CMemoryContext10CurrentPtrEv"));
  f.set_current = reinterpret_cast<void (*)(void*)>(dlsym(h, "_ZN10NRadEngine14CMemoryContext10SetCurrentEPNS_10CAllocatorE"));
  f.current = reinterpret_cast<void* (*)()>(dlsym(h, "_ZN10NRadEngine14CMemoryContext7CurrentEv"));
  dlclose(h);
  if (!f.block_allocator || !f.current_ptr || !f.set_current || !f.current) return false;
  *out = f;
  return true;
}

InstallResult InstallVoipDropAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr, const VoipDropFns& fns) {
  InstallResult r;
  if (OffFilePresent("voipdrop.off")) {
    const NevrLogField f[] = {{"why", "voipdrop.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "voipdrop off", f, 1);
    r.reason = "off_file";
    return r;
  }
  if (!fns.block_allocator || !fns.current_ptr || !fns.set_current || !fns.current) {
    const NevrLogField f[] = {{"why", "CMemBlock::Allocator / CMemoryContext::CurrentPtr / SetCurrent / Current not exported"}};
    Log(NEVR_LOG_ERROR, "voipdrop refused: nothing changed", f, 1);
    r.reason = "export_missing";
    return r;
  }
  g_f = fns;
  r = InstallImportHook(kSymbol, base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&RecvWrapper), &g_real, real_vaddr);
  char slot[24], real[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(slot_vaddr));
  std::snprintf(real, sizeof real, "0x%llx", static_cast<unsigned long long>(real_vaddr));
  const NevrLogField f[] = {{"slot_vaddr", slot}, {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  if (r.status == SwapStatus::kOk)
    Log(NEVR_LOG_INFO, "voipdrop on: ReceiveVoip drops a packet whose voice table page cannot be allocated", f, 4);
  else
    LogSlotRefusal("voipdrop refused: nothing changed (or restore failed, see status)", base, slot_vaddr, real_vaddr, r);
  return r;
}

bool VoipDropWordPinsMatch(std::uintptr_t base) { return CheckWordPins("voipdrop", base, kVoipDropWordPins, kVoipDropWordPinCount); }

InstallResult InstallVoipDrop(std::uintptr_t base) {
  if (!VoipDropWordPinsMatch(base)) {
    InstallResult r;
    r.reason = "word_pin_mismatch";
    return r;
  }
  VoipDropFns fns{};
  if (!ResolveVoipDropFns(&fns)) {
    const NevrLogField f[] = {{"why", "CMemBlock::Allocator / CMemoryContext::CurrentPtr / SetCurrent / Current not exported"}};
    Log(NEVR_LOG_ERROR, "voipdrop refused: nothing changed", f, 1);
    InstallResult r;
    r.reason = "export_missing";
    return r;
  }
  return InstallVoipDropAt(base, kVoipDropSlot, kVoipDropReal, fns);
}

std::uint32_t VoipDropCount() { return g_drops.load(std::memory_order_relaxed); }

void SetVoipDropDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void ResetVoipDropForTest() {
  g_drops.store(0);
  g_lines.store(0);
  g_totals.store(0);
  g_real.store(0);
  g_f = VoipDropFns{};
}

}  // namespace q15t

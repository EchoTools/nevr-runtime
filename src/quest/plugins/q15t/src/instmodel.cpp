#include "instmodel.h"

#include <cstdio>
#include <cstring>

#include <atomic>

#include "got_swap.h"
#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"
#include "word_pins.h"

namespace q15t {
namespace {

// ELF vaddrs in the pinned build (disassembly VA - 0x100000; docs/quest/q15t/modules/instmodel.md has the table with the bytes).
constexpr std::uintptr_t kCtorVaddr = 0x19078dc;      // CGInstancedModelResource::CGInstancedModelResource(CPresenceInit const&), disassembly 0x1a078dc
constexpr std::uintptr_t kSlotVaddr = 0x36c5c68;      // the GOT word of that constructor: the PLT stub at disassembly 0xfdcb00 is ldr x17,[x16,#0xc68]; br x17
constexpr std::uintptr_t kStepWordVaddr = 0x1907958;  // disassembly 0x1a07958: mov w20,#0x20; the register every growth-step store reads
constexpr std::uint32_t kStockCtorFirstWord = 0xf81e0ff4;  // str x20,[sp,#-0x20]! (the constructor's first instruction)
constexpr std::uint32_t kStockStepWord = 0x52800414;       // mov w20,#0x20

// The eight CTableXT members, by the offset of their CMemBlock inside the object. Each member's growth step is at member + 0x20 and
// its capacity at member + 0x28 (the same layout custctx.cpp's DrawTable reads). disassembly 0x1a078dc stores the step at
// 0x98, 0xd0, 0x108, 0x140, 0x178, 0x1b0, 0x1e8 and 0x220 (each of its CMemBlock constructions is at 0x78, 0xb0, ... 0x200).
constexpr std::uintptr_t kMembers[] = {0x78, 0xb0, 0xe8, 0x120, 0x158, 0x190, 0x1c8, 0x200};
constexpr std::uintptr_t kStepOffset = 0x20;
constexpr std::uint64_t kStockStep = 0x20;  // the value the stock constructor stores
constexpr std::uint64_t kNewStep = 1;
constexpr unsigned kLogLines = 16;  // INFO lines per process; the first line over the cap says so, then silence

// The stock constructor, stored before the GOT word is swapped (import_hook.cpp), so the wrapper never reads it null.
std::atomic<std::uintptr_t> g_real{0};
std::atomic<unsigned> g_lines{0};

// One line per constructed object is too many; the first kLogLines are written, then one WARNING that the cap was reached.
bool Budget() {
  const unsigned n = g_lines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == kLogLines + 1) {
    const NevrLogField f[] = {{"cap", "16"}};
    Log(NEVR_LOG_WARNING, "instmodel log cap reached: further steps are applied but not logged", f, 1);
  }
  return n <= kLogLines;
}

// Sets the growth step of each member whose stock value is still in place. A member that holds another value is left as it is and
// counted in *mismatch (one WARNING line names the first one).
unsigned ApplySteps(unsigned char* self, unsigned* mismatch) {
  unsigned set = 0;
  for (const std::uintptr_t member : kMembers) {
    const std::uintptr_t at = member + kStepOffset;
    std::uint64_t have = 0;
    std::memcpy(&have, self + at, sizeof have);
    if (have != kStockStep) {
      if (*mismatch == 0) {
        char off[24], val[24];
        std::snprintf(off, sizeof off, "0x%llx", static_cast<unsigned long long>(at));
        std::snprintf(val, sizeof val, "0x%llx", static_cast<unsigned long long>(have));
        const NevrLogField f[] = {{"offset", off}, {"found", val}, {"expected", "0x20"}};
        Log(NEVR_LOG_WARNING, "instmodel: a growth step does not hold the stock value, it is left as it is", f, 3);
      }
      ++*mismatch;
      continue;
    }
    const std::uint64_t one = kNewStep;
    std::memcpy(self + at, &one, sizeof one);
    ++set;
  }
  return set;
}

}  // namespace

void SetInstModelRealForTest(InstModelCtorFn real) { g_real.store(reinterpret_cast<std::uintptr_t>(real)); }

void ResetInstModelForTest() {
  g_real.store(0);
  g_lines.store(0);
}

// The replacement constructor: the stock constructor (its vtables, its CMemBlocks, its allocator context and every other field), then the
// eight steps. Nothing between the stock constructor's stores reads a step, so the object after this call is the object the patched
// constructor (mov w20,#1) builds.
void InstModelCtorWrapper(void* self, const void* init) {
  const auto real = reinterpret_cast<InstModelCtorFn>(g_real.load(std::memory_order_acquire));
  real(self, init);
  unsigned mismatch = 0;
  const unsigned set = ApplySteps(static_cast<unsigned char*>(self), &mismatch);
  if (Budget()) {
    char n[16], m[16];
    std::snprintf(n, sizeof n, "%u", set);
    std::snprintf(m, sizeof m, "%u", mismatch);
    const NevrLogField f[] = {{"steps_set", n}, {"steps_mismatch", m}, {"step", "1"}};
    Log(NEVR_LOG_INFO, "instmodel: CGInstancedModelResource growth steps set", f, 3);
  }
}

InstallResult InstallInstModel(std::uintptr_t base) {
  InstallResult r;
  if (ModuleSwitchPresent("instmodel.off")) {
    const NevrLogField f[] = {{"why", "instmodel.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "instmodel off", f, 1);
    r.reason = "off_file";
    return r;
  }
  if (base == 0) {
    r.reason = "no_base";
    return r;
  }
  // The stock words must read back before anything is swapped: the constructor's first instruction and the step constant.
  const WordPin pins[] = {{kCtorVaddr, kStockCtorFirstWord}, {kStepWordVaddr, kStockStepWord}};
  if (!CheckWordPins("instmodel", base, pins, 2)) {
    r.reason = "word_pin_mismatch";
    return r;
  }
  const InstallResult ri = InstallImportHook("CGInstancedModelResource ctor (JUMP_SLOT)", base, kSlotVaddr,
                                             reinterpret_cast<std::uintptr_t>(&InstModelCtorWrapper), &g_real, kCtorVaddr);
  char slot[24], real_va[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(kSlotVaddr));
  std::snprintf(real_va, sizeof real_va, "0x%llx", static_cast<unsigned long long>(kCtorVaddr));
  if (ri.status == SwapStatus::kOk) {
    const NevrLogField f[] = {{"hook", "CGInstancedModelResource ctor"}, {"slot_vaddr", slot}, {"real_vaddr", real_va},
                              {"status", StatusName(ri.status)}};
    Log(NEVR_LOG_INFO, "instmodel word swapped", f, 4);
  } else {
    LogSlotRefusal("instmodel refused: the constructor's GOT word could not be swapped, nothing else is changed", base, kSlotVaddr,
                   kCtorVaddr, ri);
  }
  return ri;
}

}  // namespace q15t

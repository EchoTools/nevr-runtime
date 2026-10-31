#include "shaderstep.h"

#include <atomic>
#include <cstring>

#include "module_switch.h"
#include "pbhud_log.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// ELF vaddrs of the pinned build (disassembly VA = ELF + 0x100000); see the header for how each was read.
constexpr std::uintptr_t kSlot = 0x365ddc8;  // vtable slot 1 of CFactory<CGShaderSetResource> (disassembly 0x375ddc8)
constexpr std::uintptr_t kFn = 0x1906f4c;    // CFactory<CGShaderSetResource>::ConstructInstance (disassembly 0x1a06f4c)
constexpr char kSym[] = "_ZN10NRadEngine8CFactoryINS_19CGShaderSetResourceEE17ConstructInstanceEPvPNS_13CPresenceInitE";

// CTableXT as the object holds it: the table starts at +0x78 and its growth step is at +0x20 of the table (+0x98 of the object).
constexpr std::uintptr_t kStepOff = 0x98;
constexpr std::uint64_t kStockStep = 0x20;  // the value ConstructInstance stores (disassembly 0x1a07000 mov w8,#0x20; str x8,[x19,#0x98])
constexpr std::uint64_t kStep = 1;          // the value this module writes

using ConstructFn = void* (*)(void* self, void* obj, const void* init);
std::atomic<std::uintptr_t> g_next{0};
std::atomic<std::uint32_t> g_constructed{0}, g_stepped{0}, g_not_stock{0};

__attribute__((noinline, cold)) void LogNotStock(std::uint64_t found) {  // the first few only; the count is in the stats
  if (g_not_stock.load(std::memory_order_relaxed) > 4) return;
  Rec().X("found", found).X("expected", kStockStep)
      .Emit(NEVR_LOG_WARNING, "shaderstep: step word is not the game's 0x20; left alone");
}

void* Wrapper(void* self, void* obj, const void* init) {
  const auto next = reinterpret_cast<ConstructFn>(g_next.load(std::memory_order_acquire));
  if (next == nullptr) return obj;  // unreachable: InstallImportHook stores the real function before the swap
  void* built = next(self, obj, init);
  const std::uint32_t n = g_constructed.fetch_add(1, std::memory_order_relaxed) + 1;
  switch (ApplyShaderSetStep(built)) {
    case StepFix::kSet:
      g_stepped.fetch_add(1, std::memory_order_relaxed);
      if (n == 1) Rec().X("obj", reinterpret_cast<std::uintptr_t>(built)).U("step", kStep)
                      .Emit(NEVR_LOG_INFO, "shaderstep: first shader-set table built with growth step 1");
      break;
    case StepFix::kNotStock:
      g_not_stock.fetch_add(1, std::memory_order_relaxed);
      break;
    case StepFix::kUnreadable:
      break;
  }
  return built;
}

}  // namespace

StepFix ApplyShaderSetStep(void* obj) {
  if (obj == nullptr) return StepFix::kUnreadable;
  const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(obj) + kStepOff;
  bool ok = true;  // Rd only ever clears it
  const std::uint64_t found = Rd<std::uint64_t>(at, &ok);
  if (!ok) return StepFix::kUnreadable;
  if (found != kStockStep) {
    LogNotStock(found);
    return StepFix::kNotStock;
  }
  std::memcpy(reinterpret_cast<void*>(at), &kStep, sizeof kStep);
  bool back_ok = true;
  const std::uint64_t now = Rd<std::uint64_t>(at, &back_ok);
  return back_ok && now == kStep ? StepFix::kSet : StepFix::kUnreadable;
}

ShaderStepStats GetShaderStepStats() { return {g_constructed.load(), g_stepped.load(), g_not_stock.load()}; }

void ResetShaderStepForTest() {
  g_next = 0;
  g_constructed = g_stepped = g_not_stock = 0;
}

InstallResult InstallShaderStep(std::uintptr_t base) {
  if (ModuleSwitchPresent("shaderstep.off")) {
    const NevrLogField f[] = {{"why", "shaderstep.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "shaderstep off", f, 1);
    InstallResult r;
    r.reason = "off_file";
    return r;
  }
  const InstallResult r =
      InstallImportHook(kSym, base, kSlot, reinterpret_cast<std::uintptr_t>(&Wrapper), &g_next, kFn);
  Rec rec;
  rec.X("slot_elf_vaddr", kSlot).X("fn_elf_vaddr", kFn).S("status", StatusName(r.status)).S("reason", r.reason);
  if (r.status == SwapStatus::kOk)
    rec.U("step", kStep).Emit(NEVR_LOG_INFO, "shaderstep on: CGShaderSetResource table growth step set to 1 at construction");
  else if (r.status == SwapStatus::kRestoreFailed)
    rec.Emit(NEVR_LOG_ERROR, "shaderstep: on, but the slot page's old protection could not be restored");
  else
    rec.Emit(NEVR_LOG_ERROR, "shaderstep: OFF; nothing installed");
  return r;
}

}  // namespace q15t

#include "meshstep.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// The growth step the stock constructor stores is 32; this reconstruction stores 1.
constexpr std::uint64_t kGrowStep = 1;

// The member blocks (CMemBlock at each offset, 0x38 bytes) whose growth step the stock constructor writes at block + 0x20. The
// order is the stock constructor's: the CMemBlock constructor, then the step, then the two zeroed words at block + 0x28 and + 0x30.
constexpr std::uintptr_t kTableOffsets[] = {0x2b0, 0x2e8, 0x320, 0x358, 0x390, 0x3c8, 0x400, 0x438, 0x470, 0x4a8, 0x4e0};

// The data-page words of the GOT slot and the wrapper's state. Written once, before the slot is swapped, and only read after.
MeshStepFns g_fns{};
std::uint64_t g_vt_presence = 0;  // NRadEngine::CPresence vtable (the stored word + 0x10)
std::uint64_t g_vt_mesh = 0;      // CGMeshListResource vtable (the stored word + 0x10)
std::atomic<std::uintptr_t> g_real{0};  // the stock constructor's address, as InstallImportHook records it
std::atomic<unsigned> g_builds{0};
std::atomic<bool> g_logged_first{false};

void Put64(std::uint8_t* self, std::uintptr_t off, std::uint64_t v) { std::memcpy(self + off, &v, sizeof v); }

std::uint64_t Get64(const std::uint8_t* p, std::uintptr_t off) {
  std::uint64_t v = 0;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

// CGMeshListResource::CGMeshListResource(CPresenceInit const&), the stock order of stores and calls, with the step 1.
void BuildMeshList(std::uint8_t* self, const std::uint8_t* init) {
  Put64(self, 0x10, 0);
  Put64(self, 0x18, 0);
  Put64(self, 0x00, g_vt_presence);
  Put64(self, 0x08, 0);
  Put64(self, 0x20, Get64(init, 0x08));
  Put64(self, 0x28, Get64(init, 0x10));
  Put64(self, 0x30, g_fns.current());
  Put64(self, 0x38, ~std::uint64_t{0});
  Put64(self, 0x40, 0);
  Put64(self, 0x48, 0);
  Put64(self, 0x50, 0);
  Put64(self, 0x58, 0);
  Put64(self, 0x00, g_vt_mesh);
  Put64(self, 0x60, 7);
  Put64(self, 0x68, 0);
  g_fns.list_data_ctor(self + 0x70);
  for (const std::uintptr_t off : kTableOffsets) {
    g_fns.memblock_ctor(self + off, 0, 0, g_fns.current_ptr());
    Put64(self, off + 0x20, kGrowStep);
    Put64(self, off + 0x28, 0);
    Put64(self, off + 0x30, 0);
  }
}

// The swapped GOT word points here: the thunk calls it with x0 = this and x1 = the CPresenceInit.
void MeshListCtorWrapper(void* self, const void* init) {
  BuildMeshList(static_cast<std::uint8_t*>(self), static_cast<const std::uint8_t*>(init));
  g_builds.fetch_add(1, std::memory_order_relaxed);
  if (!g_logged_first.exchange(true, std::memory_order_relaxed)) {
    const NevrLogField f[] = {{"growth_step", "1"}, {"stock_growth_step", "32"}};
    Log(NEVR_LOG_INFO, "meshstep: first CGMeshListResource built with the mesh-list table step", f, 2);
  }
}

}  // namespace

InstallResult InstallMeshStepAt(std::uintptr_t base, const MeshStepAddrs& a, const MeshStepFns& fns) {
  InstallResult r;
  if (ModuleSwitchPresent("meshstep.off")) {
    const NevrLogField f[] = {{"why", "meshstep.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "meshstep off", f, 1);
    r.reason = "off_file";
    return r;
  }
  if (base == 0) {
    r.reason = "no_base";
    return r;
  }
  if (fns.current == nullptr || fns.current_ptr == nullptr || fns.list_data_ctor == nullptr || fns.memblock_ctor == nullptr) {
    const NevrLogField f[] = {{"why", "a helper of the constructor is not resolved"}};
    Log(NEVR_LOG_ERROR, "meshstep refused: nothing changed", f, 1);
    r.reason = "helper_missing";
    return r;
  }
  if (!CheckWordPins("meshstep", base, kMeshStepWordPins, kMeshStepWordPinCount)) {
    r.reason = "word_pin_mismatch";
    return r;
  }
  bool ok_presence = true;  // Rd() only ever clears it
  bool ok_mesh = true;
  const std::uint64_t vp = Rd<std::uint64_t>(base + a.vtbl_presence, &ok_presence);
  const std::uint64_t vm = Rd<std::uint64_t>(base + a.vtbl_mesh, &ok_mesh);
  if (!ok_presence || !ok_mesh || vp == 0 || vm == 0) {
    const NevrLogField f[] = {{"why", "a vtable word of the constructor is unreadable or zero"}};
    Log(NEVR_LOG_ERROR, "meshstep refused: nothing changed", f, 1);
    r.reason = "vtable_unreadable";
    return r;
  }
  g_fns = fns;
  g_vt_presence = vp + 0x10;
  g_vt_mesh = vm + 0x10;
  const auto wrapper = reinterpret_cast<std::uintptr_t>(&MeshListCtorWrapper);
  r = InstallImportHook("", base, a.got_slot, wrapper, &g_real, a.ctor);
  if (r.status == SwapStatus::kRestoreFailed) {
    // The word was written; put it back so the stock constructor runs again.
    SwapWord(base + a.got_slot, wrapper, base + a.ctor);
  }
  if (r.status != SwapStatus::kOk) {
    char slot[24];
    std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(a.got_slot));
    const NevrLogField f[] = {{"slot_vaddr", slot}, {"status", StatusName(r.status)}, {"reason", r.reason}};
    Log(NEVR_LOG_ERROR, "meshstep refused: nothing changed", f, 3);
    return r;
  }
  char slot[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(a.got_slot));
  const NevrLogField f[] = {{"slot_vaddr", slot}, {"growth_step", "1"}};
  Log(NEVR_LOG_INFO, "meshstep on: CGMeshListResource constructor swapped through its GOT word", f, 2);
  return r;
}

InstallResult InstallMeshStep(std::uintptr_t base) {
  const MeshStepFns fns = {
      reinterpret_cast<std::uint64_t (*)()>(base + kMeshStepPinned.current_thunk),
      reinterpret_cast<void* (*)()>(base + kMeshStepPinned.current_ptr_thunk),
      reinterpret_cast<void (*)(void*)>(base + kMeshStepPinned.list_data_ctor),
      reinterpret_cast<void (*)(void*, std::uint64_t, std::uint64_t, void*)>(base + kMeshStepPinned.memblock_ctor),
  };
  if (base == 0) {
    InstallResult r;
    r.reason = "no_base";
    return r;
  }
  return InstallMeshStepAt(base, kMeshStepPinned, fns);
}

MeshStepStats GetMeshStepStats() { return {g_builds.load(std::memory_order_relaxed)}; }

void ResetMeshStepForTest() {
  g_fns = MeshStepFns{};
  g_vt_presence = g_vt_mesh = 0;
  g_real.store(0, std::memory_order_relaxed);
  g_builds.store(0, std::memory_order_relaxed);
  g_logged_first.store(false, std::memory_order_relaxed);
}

}  // namespace q15t

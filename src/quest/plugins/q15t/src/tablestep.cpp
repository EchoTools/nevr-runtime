#include "tablestep.h"

#include <atomic>
#include <cstdint>

#include "module_switch.h"
#include "pbhud_log.h"
#include "plugin_log.h"
#include "word_pins.h"

namespace q15t {
namespace {

// ELF vaddrs of the pinned build (ReVault VA = ELF + 0x100000).
//
// The stock function, ReVault libr15 0x1a0707c (CFactory<CGShaderSetResource>::CreateInstance, 292 bytes, raw decompilation and
// disassembly read this session). Reconstructed as C++:
//   void* CreateInstance(void* unused, CPresenceInit* init) {   // x0 only passes through to Current(); x1 is init
//     obj = CMemoryContext::Current()->alloc(0xb0);              // 0x1a07088..0x1a07098
//     if (init == nullptr) init = DefaultInit();                 // 0x1a070a0..0x1a070b8 (guard-once)
//     obj->presence = { vptr, fields from init };                // 0x1a070bc..0x1a070e0
//     obj->resource = { vptr, ... };                             // 0x1a070e4..0x1a07118
//     CMemBlock::CMemBlock(&obj->table, 0, 0, CMemoryContext::CurrentPtr());   // 0x1a0712c, table at obj+0x78
//     obj->table.step = 0x20;                                    // 0x1a07130 mov w8,#0x20; 0x1a07138 str x8,[x19,#0x98]
//     return obj;
//   }
// The table is CTableXT, laid out as custctx's DrawTable: step at +0x20 of the table, so +0x98 of the object. The growth step
// is the only field this module changes.
//
// The module does not reimplement the allocation, the constructors or the guard. It wraps the stock function: it calls it, and
// after it returns sets the step of the object it made from 0x20 to 1. The stock store at 0x1a07138 is the last write to the
// step before the return, so the result is the stock function with the immediate changed to 1 (the mov w8,#1 of the row).
constexpr std::uintptr_t kCreateInstance = 0x190707c;
constexpr std::uintptr_t kStepWord = 0x1907130;  // mov w8,#0x20: the code pin
constexpr std::uint32_t kStepWordWant = 0x52800408;
constexpr std::uintptr_t kStepOffset = 0x98;  // CTableXT growth step within the CGShaderSetResource object
constexpr std::uint64_t kStock = 0x20, kWant = 1;

// The data word that holds CreateInstance's address, as an ELF vaddr: the CFactory<CGShaderSetResource> vtable word at 0x365ddd0, the
// only dynamic relocation of the pinned libr15.so whose target is CreateInstance (`readelf -r`: R_AARCH64_ABS64 at 0x365ddd0 to 0x190707c).
// It is swapped through InstallImportHook, checked against base + kCreateInstance, so a slot that holds something else is refused.
constexpr int kSlotCount = 1;
constexpr std::uintptr_t kSlots[kSlotCount] = {0x365ddd0};

using CreateFn = void* (*)(void* unused, void* init);

// Each slot has its own stored original. InstallImportHook clears the word it stored when a slot is refused, so a shared word would
// unhook the slots that installed before it.
std::atomic<std::uintptr_t> g_next[kSlotCount]{};
std::atomic<unsigned> g_rewritten{0}, g_unexpected{0};
std::atomic<bool> g_first_logged{false}, g_first_unexpected_logged{false};

void* Wrap(int i, void* unused, void* init) {
  // Stored before the slot was swapped (InstallImportHook), so the original is set whenever this runs.
  const auto next = reinterpret_cast<CreateFn>(g_next[i].load(std::memory_order_acquire));
  void* obj = next(unused, init);
  if (obj == nullptr) return obj;  // the stock path's own result, unchanged
  // The object was just built by the stock function on this thread: the step word is ours to read and, if it is the stock value, write.
  auto* step = reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(obj) + kStepOffset);
  if (*step == kStock) {
    *step = kWant;
    g_rewritten.fetch_add(1, std::memory_order_relaxed);
    if (!g_first_logged.exchange(true, std::memory_order_relaxed)) {
      Rec().X("slot", static_cast<unsigned long long>(i)).U("from", kStock).U("to", kWant)
          .Emit(NEVR_LOG_INFO, "tablestep: first shader-set table step set (later ones are counted, not logged)");
    }
  } else {
    g_unexpected.fetch_add(1, std::memory_order_relaxed);
    if (!g_first_unexpected_logged.exchange(true, std::memory_order_relaxed)) {
      Rec().X("slot", static_cast<unsigned long long>(i)).U("step", *step)
          .Emit(NEVR_LOG_WARNING, "tablestep: table step is not the stock value, left alone (logged once)");
    }
  }
  return obj;
}

template <int I>
void* Wrapper(void* unused, void* init) {
  return Wrap(I, unused, init);
}

using WrapperFn = void* (*)(void*, void*);
constexpr WrapperFn kWrappers[kSlotCount] = {&Wrapper<0>};

}  // namespace

TableStepStats GetTableStepStats() { return {g_rewritten.load(), g_unexpected.load()}; }

void ResetTableStepForTest() {
  for (auto& n : g_next) n = 0;
  g_rewritten = 0;
  g_unexpected = 0;
  g_first_logged = false;
  g_first_unexpected_logged = false;
}

InstallResult InstallTableStep(std::uintptr_t base) {
  InstallResult result;
  if (base == 0) {
    result.reason = "no_base";
    Log(NEVR_LOG_ERROR, "tablestep not installed: no game base");
    return result;
  }
  if (ModuleSwitchPresent("tablestep.off")) {
    result.status = SwapStatus::kOk;
    result.reason = "tablestep_off";
    Log(NEVR_LOG_INFO, "tablestep off: tablestep.off present, nothing changed");
    return result;
  }
  const WordPin pins[] = {{kStepWord, kStepWordWant}};
  if (!CheckWordPins("tablestep", base, pins, 1)) {
    result.status = SwapStatus::kUnexpectedOriginal;
    result.reason = "word_pin_mismatch";
    return result;
  }

  int installed = 0;
  SwapStatus first_failure = SwapStatus::kOk;
  const char* first_reason = "";
  for (int i = 0; i < kSlotCount; ++i) {
    const InstallResult r = InstallImportHook("tablestep", base, kSlots[i], reinterpret_cast<std::uintptr_t>(kWrappers[i]),
                                              &g_next[i], kCreateInstance);
    Rec rec;
    rec.X("slot_elf_vaddr", kSlots[i]).X("fn_elf_vaddr", kCreateInstance).S("status", StatusName(r.status)).S("reason", r.reason);
    if (r.status == SwapStatus::kOk) {
      ++installed;
      rec.Emit(NEVR_LOG_INFO, "tablestep: slot swapped");
    } else if (r.status == SwapStatus::kRestoreFailed) {
      ++installed;  // the word was written; only the page's protection could not be put back (the caller logged it)
      rec.Emit(NEVR_LOG_ERROR, "tablestep: slot swapped, but the page's old protection could not be restored");
    } else {
      if (first_failure == SwapStatus::kOk) {
        first_failure = r.status;
        first_reason = r.reason;
      }
      rec.Emit(NEVR_LOG_WARNING, "tablestep: slot refused, its calls stay stock");
    }
  }

  if (installed == 0) {
    result.status = first_failure;
    result.reason = first_reason;
    Log(NEVR_LOG_ERROR, "tablestep: OFF; no slot installed, nothing changed");
    return result;
  }
  Rec().U("slots", static_cast<unsigned long long>(installed)).U("of", static_cast<unsigned long long>(kSlotCount))
      .Emit(NEVR_LOG_INFO, "tablestep: on (shader-set table growth step 32 -> 1)");
  result.status = SwapStatus::kOk;
  return result;
}

}  // namespace q15t

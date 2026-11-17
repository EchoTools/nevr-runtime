#include "runtime/combat/barrier_copies.h"

#include <atomic>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::barrier_copies {
namespace {

std::atomic_bool g_registered{false};

}  // namespace

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Patch rows[kRowCount] = {
      {kSmokeScript, kParamSymbolRva, kParamSymbolStock, kSmokeParamSymbol.data(), sizeof kParamSymbolStock,
       "BARRIER COPY: UMBRAL VEIL marker SMOK (param node symbol)"},
      {kSmokeScript, kPrimingShowRvaA, kShowOn, kShowOff, sizeof kShowOn,
       "BARRIER COPY: UMBRAL VEIL priming hologram off (ShowModel at 0x6668)"},
      {kSmokeScript, kPrimingShowRvaB, kShowOn, kShowOff, sizeof kShowOn,
       "BARRIER COPY: UMBRAL VEIL priming hologram off (ShowModel at 0x74F8)"},
      {kPointScript, kParamSymbolRva, kParamSymbolStock, kPointParamSymbol.data(), sizeof kParamSymbolStock,
       "BARRIER COPY: NEUTRON marker POIN (param node symbol)"},
      {kPointScript, kPrimingShowRvaA, kShowOn, kShowOff, sizeof kShowOn,
       "BARRIER COPY: NEUTRON priming hologram off (ShowModel at 0x6668)"},
      {kPointScript, kPrimingShowRvaB, kShowOn, kShowOff, sizeof kShowOn,
       "BARRIER COPY: NEUTRON priming hologram off (ShowModel at 0x74F8)"},
  };
  nevr_script_hooks::Register(rows, kRowCount);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] barrier copies fix registered copies=2 patches=%llu (the UMBRAL VEIL and NEUTRON barrier copies write "
      "their wall marker and show no priming hologram; the patches are written when each copy loads, and a changed byte "
      "refuses the copy, which then runs stock)",
      static_cast<unsigned long long>(kRowCount));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() { g_registered = false; }
#endif

}  // namespace nevr_combat::barrier_copies

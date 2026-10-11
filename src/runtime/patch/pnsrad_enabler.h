/* SYNTHESIS -- custom tool code, not from binary */
#pragma once
#include <cstdint>
namespace nevr_pnsrad_enabler {
    void Init(uintptr_t base_addr);
    void Shutdown();

    /// pnsrad.dll's module base once it has loaded and been patched, 0 before
    /// that. Lets other patches (e.g. mic_provider's CSysDLL_GetSymbolHook
    /// extension) identify a GetProcAddress/CSysDLL_GetSymbol call as
    /// targeting pnsrad.dll without a second DLL-notification registration.
    uintptr_t GetModuleBase();
}

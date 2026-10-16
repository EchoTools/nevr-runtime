/* SYNTHESIS -- custom tool code, not from binary */
#pragma once
#include <cstdint>
namespace nevr_pnsrad_enabler {
    void Init(uintptr_t base_addr);
    void Shutdown();
    void InstallPendingDiagnostics();

    /// Patches the pnsrad.dll and pnsradmatchmaking.dll loads that were recorded but not patched when their
    /// LoadLibrary returned (a load outside LoadLibrary*, or one returned while its thread held the loader
    /// lock), with a warning for each. Called from the frame tick.
    void ApplyLoadsMissedAtLibraryReturn();

    /// pnsrad.dll's module base once it has loaded and been patched, 0 before
    /// that. Lets other patches (e.g. mic_provider's CSysDLL_GetSymbolHook
    /// extension) identify a GetProcAddress/CSysDLL_GetSymbol call as
    /// targeting pnsrad.dll without a second DLL-notification registration.
    uintptr_t GetModuleBase();
}

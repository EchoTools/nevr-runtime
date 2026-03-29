#pragma once

#include "common/pch.h"
#include "common/hooking.h"
#include "common/echovr_functions.h"
#include "process_mem.h"
#include "patch_addresses.h"

/// <summary>
/// Helper function to apply a memory patch at a specific offset from the game base address.
/// </summary>
/// <param name="offset">The offset from the game base address.</param>
/// <param name="patchData">Pointer to the patch data.</param>
/// <param name="patchSize">Size of the patch in bytes.</param>
static inline VOID ApplyPatch(uintptr_t offset, const BYTE* patchData, size_t patchSize) {
  ProcessMemcpy(EchoVR::g_GameBaseAddress + offset, const_cast<BYTE*>(patchData), patchSize);
}

/// <summary>
/// Patches a given function pointer with an hook function (matching the equivalent function signature as the original).
/// </summary>
/// <param name="ppPointer">The function to detour.</param>
/// <param name="pDetour">The function hook to use as a detour.</param>
/// <returns>None</returns>
template <typename T>
inline VOID PatchDetour(T* ppPointer, PVOID pDetour) {
  Hooking::Attach(reinterpret_cast<PVOID*>(ppPointer), pDetour);
}

/// Layout must match the declaration in gameserver.cpp (used via GetProcAddress).
struct NevRUPnPConfig {
  BOOL   enabled;
  UINT16 port;
  CHAR   internalIp[46];
  CHAR   externalIp[46];
};

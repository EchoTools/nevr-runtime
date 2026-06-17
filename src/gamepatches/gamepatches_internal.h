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

/// Install a MinHook detour on a function pointer.
/// Returns TRUE if the hook was installed, FALSE on failure.
template <typename T>
inline BOOL PatchDetour(T* ppPointer, PVOID pDetour) {
  return Hooking::Attach(reinterpret_cast<PVOID*>(ppPointer), pDetour);
}

/// Layout must match the declaration in gameserver.cpp (used via GetProcAddress).
struct NevRUPnPConfig {
  BOOL   enabled;
  UINT16 port;
  CHAR   internalIp[46];
  CHAR   externalIp[46];
};

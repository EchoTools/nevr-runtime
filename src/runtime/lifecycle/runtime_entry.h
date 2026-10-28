#pragma once

// The runtime's side of the host contract (core/runtime_abi.h, docs/adr/0008-bugsplat-host-and-nevr-runtime.md).
// nevr-runtime.dll exports one function, NEVR_GetRuntimeApi, which the host (BugSplat64.dll) resolves by name
// after it loaded this DLL at the rendezvous. tools/verify_pe_contract.py checks that the export is there.

#include "core/runtime_abi.h"

namespace nevr_runtime_entry {

/// The descriptor the host validates: `initialize`, `game_main` and the export table. Static storage, never freed.
const NevrRuntimeApi* GetRuntimeApi();

}  // namespace nevr_runtime_entry

/// The runtime's single versioned entry point (nevr_runtime_abi::kGetRuntimeApiExport).
extern "C" __declspec(dllexport) const NevrRuntimeApi* NEVR_GetRuntimeApi();

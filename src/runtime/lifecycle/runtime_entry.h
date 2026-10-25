#pragma once

// The runtime's side of the host contract (core/runtime_abi.h, docs/adr/0008-bugsplat-host-and-nevr-runtime.md).
// The runtime is compiled into the host's image, so the descriptor is handed to the host's loader
// as a function pointer instead of an export: the DLL's export table stays the set
// tools/verify_pe_contract.py pins.

#include "core/runtime_abi.h"

namespace nevr_runtime_entry {

/// The descriptor the host validates: `initialize` and `game_main`. Static storage, never freed.
const NevrRuntimeApi* GetRuntimeApi();

}  // namespace nevr_runtime_entry

// Installs a wrapper on one JUMP_SLOT import of the game library: resolves the runtime's real function, validates the
// slot's current word, swaps it (data page only, got_swap.h) and stores the real address for the wrapper to call.
#pragma once

#include <atomic>
#include <cstdint>

#include "got_swap.h"

namespace q15t {

struct InstallResult {
  SwapStatus status = SwapStatus::kNullSlot;
  const char* reason = "";      // a fixed token for a refusal that is not a SwapStatus
  bool real_resolved = false;   // the runtime symbol was found by name (else the slot word was accepted by mapping)
};

// `symbol`: the runtime's export, resolved with dlsym (RTLD_DEFAULT, then libvrapi.so with RTLD_NOLOAD).
// `slot_vaddr`: the slot's ELF vaddr in the pinned build; `base` the game library's load base.
// `real_out`: set to the real function's address before the swap and cleared again if the swap fails.
// `expected_vaddr` != 0: the real function lives in the game library itself at that ELF vaddr (the slot's symbol value in the
// pinned build); the slot must hold base + expected_vaddr and no name lookup is made.
InstallResult InstallImportHook(const char* symbol, std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t wrapper,
                                std::atomic<std::uintptr_t>* real_out, std::uintptr_t expected_vaddr = 0);

}  // namespace q15t

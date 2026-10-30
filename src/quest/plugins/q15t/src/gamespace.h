// Walks CR15Game's two gamespace tables (build 4987566) for component systems. Game thread only; every read is a
// SafeRead, so a stale or half-built table is a skipped entry, never a fault. (q15t the source build's gunconnect FindCs.)
#pragma once

#include <cstdint>

namespace q15t {

struct CompSys {
  std::uintptr_t gs = 0;      // the gamespace
  std::uintptr_t cs = 0;      // the component system
  int table = 0;              // 0: game+0x1f48 (gets exclusive updates), 1: game+0x1f88
  std::uint64_t gs_name = 0;
};

// Every component system in both tables whose key is `key` and whose vptr word is `vptr_abs` (an absolute address:
// base + the ELF vaddr), and whose cs+0x80 is its gamespace. At most `max` entries; returns the count.
int FindComponentSystems(std::uintptr_t game, std::uint64_t key, std::uintptr_t vptr_abs, CompSys* out, int max);

}  // namespace q15t

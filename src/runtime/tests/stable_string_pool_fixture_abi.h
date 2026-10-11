// Shared layout between test_stable_string_pool and the fixture DLL that
// compiles the production stable string pool source.
#pragma once

#include <cstddef>

namespace nevr::lifecycle::test {

// Updated in place by the fixture DLL from its replacement operator new/delete
// and from DllMain, so the values are valid after the DLL is unloaded. Memory
// is host-owned; the fixture never frees it.
struct FixtureObservation {
  bool detached = false;
  // Heap blocks allocated by the production pool while InternStableCStr ran.
  std::size_t ownedBlocksAllocated = 0;
  // Frees of any of those blocks, whenever they happen (including during unload).
  std::size_t ownedBlocksFreed = 0;
  // Whether the pointer returned to the caller lies inside one of those blocks.
  bool publishedPointerInOwnedBlock = false;
  // Frees of the block that contains the published pointer.
  std::size_t publishedBlockFreed = 0;
};

}  // namespace nevr::lifecycle::test

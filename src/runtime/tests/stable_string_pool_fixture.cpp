// Fixture DLL that compiles the production stable string pool source and
// records, through replacement global allocation functions, which heap blocks
// the pool owns and whether any of them is freed. The test reads the counters
// from host-owned memory, so it never dereferences storage after unload.
#include <windows.h>

#include <cstddef>
#include <new>

#include "runtime/lifecycle/stable_string_pool.h"
#include "runtime/tests/stable_string_pool_fixture_abi.h"

namespace {

using nevr::lifecycle::test::FixtureObservation;

constexpr std::size_t kMaxTrackedBlocks = 256;

struct TrackedBlock {
  void* address = nullptr;
  std::size_t size = 0;
  bool containsPublishedPointer = false;
};

FixtureObservation* g_observation = nullptr;
bool g_trackingAllocations = false;
TrackedBlock g_blocks[kMaxTrackedBlocks];

void* AllocateBlock(std::size_t size) {
  void* block = HeapAlloc(GetProcessHeap(), 0, size == 0 ? 1 : size);
  if (block == nullptr) throw std::bad_alloc();
  if (g_trackingAllocations && g_observation != nullptr) {
    for (TrackedBlock& slot : g_blocks) {
      if (slot.address == nullptr) {
        slot.address = block;
        slot.size = size;
        slot.containsPublishedPointer = false;
        ++g_observation->ownedBlocksAllocated;
        return block;
      }
    }
    HeapFree(GetProcessHeap(), 0, block);
    throw std::bad_alloc();  // the table is full: refuse rather than lose track of a block
  }
  return block;
}

void ReleaseBlock(void* block) noexcept {
  if (block == nullptr) return;
  if (g_observation != nullptr) {
    for (TrackedBlock& slot : g_blocks) {
      if (slot.address == block) {
        ++g_observation->ownedBlocksFreed;
        if (slot.containsPublishedPointer) ++g_observation->publishedBlockFreed;
        slot.address = nullptr;
        break;
      }
    }
  }
  HeapFree(GetProcessHeap(), 0, block);
}

void MarkBlockContaining(const void* pointer) noexcept {
  if (g_observation == nullptr) return;
  const auto target = reinterpret_cast<std::size_t>(pointer);
  for (TrackedBlock& slot : g_blocks) {
    if (slot.address == nullptr) continue;
    const auto start = reinterpret_cast<std::size_t>(slot.address);
    if (target >= start && target < start + slot.size) {
      slot.containsPublishedPointer = true;
      g_observation->publishedPointerInOwnedBlock = true;
    }
  }
}

}  // namespace

void* operator new(std::size_t size) { return AllocateBlock(size); }
void* operator new[](std::size_t size) { return AllocateBlock(size); }
void operator delete(void* block) noexcept { ReleaseBlock(block); }
void operator delete[](void* block) noexcept { ReleaseBlock(block); }
void operator delete(void* block, std::size_t) noexcept { ReleaseBlock(block); }
void operator delete[](void* block, std::size_t) noexcept { ReleaseBlock(block); }

extern "C" __declspec(dllexport) void StableStringPoolFixtureObserve(FixtureObservation* observation) {
  g_observation = observation;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_DETACH && g_observation != nullptr) g_observation->detached = true;
  return TRUE;
}

extern "C" __declspec(dllexport) const char* StableStringPoolFixtureIntern(const char* value) {
  if (value == nullptr) return nullptr;
  g_trackingAllocations = true;
  const auto result = nevr::lifecycle::InternStableCStr(value);
  g_trackingAllocations = false;
  if (result.status != nevr::lifecycle::InternStatus::kSuccess) return nullptr;
  MarkBlockContaining(result.pointer);
  return result.pointer;
}

// bvh (source build the source build's bvh + the source build's bvh_core; rows R20-R25; on by default in the source, option Q15T_BVH): Quest's stubbed CBVHTree gets
// a real static raycast tree, built at run time from the game's own CBVHResource (the SBVHTreeData CStaticRaycastCS passes to
// CBVHTree::Initialize), so walls block line of sight, bullets and AI. No game-derived bytes are stored anywhere.
//
// Six JUMP_SLOT wrappers, data-page swaps only (no code word is written):
//   R20 CBVHTree::Initialize(this, SBVHTreeData*)           parse + validate + keep a Tree* in the 8-byte CBVHTree
//   R21 CBVHTree::Shutdown(this)                            clear it
//   R22 CBVHTree::Intersect(this, CRay*, SBVHHit*)          nearest hit, for the casts the gate allows
//   R23 CBVHTree::Occluded(this, C3Vector*, C3Vector*)      any hit on the segment
//   R24 CncaGameSpace::StaticRaycast(this, CRay*, SBVHHit*, CncaGameSpaceMT**)   the callers' gate (marks allowed casts)
//   R25 CR15TouchInteractQuery::RayQueryStatic(...)         returns false (a miss), as the stock Quest does
// R20-R23 are installed all together or not at all, and R24 with them unless the gate is switched off. Details, evidence
// (disassembly addresses and quoted words), behaviour, prediction and the deviations from the source: docs/quest/q15t/modules/bvh.md.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

struct BvhHookSlot {
  std::uintptr_t slot;  // JUMP_SLOT ELF vaddr
  std::uintptr_t fn;    // the function the slot holds in the pinned build (ELF vaddr)
};

struct BvhPins {
  BvhHookSlot init, shutdown, intersect, occluded, touch, gate;
  std::uintptr_t allowed[4];  // ELF vaddrs of the return addresses (bl + 4) of the casts the tree answers
  int n_allowed;
};

// ELF vaddrs of the pinned build (readelf -rW, a source-build note not in this tree; disassembly VA = ELF + 0x100000).
inline constexpr BvhPins kBvhPinned = {
    {0x36ec548, 0x1a45a88},                                  // R20
    {0x36eb648, 0x1a45a8c},                                  // R21
    {0x36d38f0, 0x1a45ae4},                                  // R22
    {0x36e8430, 0x1a45b38},                                  // R23
    {0x36f37d8, 0x20cf414},                                  // R25
    {0x36c0288, 0x18066e4},                                  // R24
    {0x12b2b4c, 0x12b2bc4, 0x12c0a54, 0},                    // DualStaticRaycast forward / backward, SR15PatrolSM::Update
    3};

// Option files in the plugin's data directory (/sdcard/Android/data/<pkg>/files/q15t/), looked up at install:
//   bvh.off        nothing is installed (reason "off_file")
//   bvhgate.off    no callers' gate: every cast uses the tree (the source's debug.<app>.bvhgate 0)
//   bvhtouch.keep  RayQueryStatic is left alone (the source's debug.<app>.bvhtouch 1)
// Returns kOk when R20-R23 (and R24 unless gate-off) are all replaced; otherwise nothing is left replaced and `reason` says why.
InstallResult InstallBvh(std::uintptr_t base);
InstallResult InstallBvhAt(std::uintptr_t base, const BvhPins& pins);

// True once InstallBvh succeeded: the module wallfire depends on.
bool BvhTreeInstalled();

struct BvhCounters {
  std::uint64_t trees_built;     // Initialize calls that produced a tree
  std::uint64_t trees_refused;   // Initialize calls that did not (no geometry, bad sizes, unreadable, table full)
  std::uint64_t intersect;       // casts answered from a tree
  std::uint64_t intersect_hit;
  std::uint64_t occluded;
  std::uint64_t occluded_blocked;
  std::uint64_t gated;           // casts kept off the tree (the stock miss)
  std::uint64_t touch;           // touch rays answered with a miss
  std::uint64_t stock;           // calls that went to the stock function (no tree, an unreadable argument)
};
BvhCounters BvhGetCounters();
int BvhTreeCount();  // entries in use in the tree table (cap 64)

// A TickFn (game_tick.h): at most one summary line per 10 s, only when a counter moved. One compare per frame otherwise.
void BvhTick(std::uintptr_t game);

// Test seams.
void SetBvhDirForTest(const char* dir);
void SetBvhClockForTest(double (*clock)());
void ResetBvhForTest();                       // counters, tree table, flags (not the swapped slots)
std::uintptr_t BvhLastGateRetForTest();       // the last return address the gate saw, as an ELF vaddr

}  // namespace q15t

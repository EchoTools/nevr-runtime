/* The pinned slots of the static BVH (bvh.h) and the thunk each one is hooked through.
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`; the disassembly's
 * address is the ELF address plus 0x100000). src/quest/tests/got_pinned_test.cpp checks all six JUMP_SLOT relocations against the real
 * file (`just test-quest-hooks-pinned`, which needs the store APK):
 *
 *   Initialize    JUMP_SLOT 0x36ec548 -> 0x1a45a88   CBVHTree::Initialize(const SBVHTreeData&)
 *   Shutdown      JUMP_SLOT 0x36eb648 -> 0x1a45a8c   CBVHTree::Shutdown()
 *   Intersect     JUMP_SLOT 0x36d38f0 -> 0x1a45ae4   CBVHTree::Intersect(const CRay&, SBVHHit&) const
 *   Occluded      JUMP_SLOT 0x36e8430 -> 0x1a45b38   CBVHTree::Occluded(const C3Vector&, const C3Vector&) const
 *   Touch         JUMP_SLOT 0x36f37d8 -> 0x20cf414   CR15TouchInteractQuery::RayQueryStatic(...)
 *   Gate          JUMP_SLOT 0x36c0288 -> 0x18066e4   CncaGameSpace::StaticRaycast(const CRay&, SBVHHit&, CncaGameSpaceMT*&) const
 *
 * The casts the tree answers: the return addresses (the instruction after the `bl`) of the three callers of StaticRaycast whose casts the
 * stock Quest leaves to a stubbed tree and the PC answers from the level: CR15NetRaycastUtils::DualStaticRaycast forward (0x12b2b4c) and
 * backward (0x12b2bc4) for bullets, and SR15PatrolSM::Update (0x12c0a54) for the AI.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::bvh {

struct Slot {
  std::uint64_t slot;  // the JUMP_SLOT's ELF vaddr
  std::uint64_t fn;    // the function the slot holds in the pinned build (ELF vaddr)
};

struct Pins {
  Slot init, shutdown, intersect, occluded, touch, gate;
  std::uint64_t allowed[4];  // ELF vaddrs of the return addresses of the casts the tree answers
  int allowedCount;
};

inline constexpr Pins kPins = {
    {0x36ec548ULL, 0x1a45a88ULL},                // Initialize
    {0x36eb648ULL, 0x1a45a8cULL},                // Shutdown
    {0x36d38f0ULL, 0x1a45ae4ULL},                // Intersect
    {0x36e8430ULL, 0x1a45b38ULL},                // Occluded
    {0x36f37d8ULL, 0x20cf414ULL},                // RayQueryStatic
    {0x36c0288ULL, 0x18066e4ULL},                // StaticRaycast
    {0x12b2b4cULL, 0x12b2bc4ULL, 0x12c0a54ULL, 0},  // DualStaticRaycast forward, backward, SR15PatrolSM::Update
    3};

inline constexpr const char* kInitSymbol = "_ZN10NRadEngine8CBVHTree10InitializeERKNS_12SBVHTreeDataE";
inline constexpr const char* kShutSymbol = "_ZN10NRadEngine8CBVHTree8ShutdownEv";
inline constexpr const char* kIntersectSymbol = "_ZNK10NRadEngine8CBVHTree9IntersectERKNS_4CRayERNS_7SBVHHitE";
inline constexpr const char* kOccludedSymbol = "_ZNK10NRadEngine8CBVHTree8OccludedERKNS_8C3VectorES3_";
inline constexpr const char* kTouchSymbol =
    "_ZN10NRadEngine8NRadGame22CR15TouchInteractQuery14RayQueryStaticEPKNS_15CncaGameSpaceMTERKNS_4CRayERNS0_8SMeshHitERf";
inline constexpr const char* kGateSymbol = "_ZNK10NRadEngine13CncaGameSpace13StaticRaycastERKNS_4CRayERNS_7SBVHHitERPNS_15CncaGameSpaceMTE";

// One thunk per hooked slot (callback_thunk.h: the original is a static of the instantiation).
struct InitTag {};
struct ShutTag {};
struct IntersectTag {};
struct OccludedTag {};
struct TouchTag {};
struct GateTag {};
using InitThunk = sentinel::CallbackThunk<InitTag, void(void* self, const void* data)>;
using ShutThunk = sentinel::CallbackThunk<ShutTag, void(void* self)>;
using IntersectThunk = sentinel::CallbackThunk<IntersectTag, bool(const void* self, const float* ray, std::uint8_t* hit)>;
using OccludedThunk = sentinel::CallbackThunk<OccludedTag, bool(const void* self, const float* from, const float* to)>;
using TouchThunk = sentinel::CallbackThunk<TouchTag, bool(const void*, const void*, void*, float*)>;
// kCaller: the handler is told the instruction after the game's call, which is how it tells the three casts the tree answers from the others.
using GateThunk = sentinel::CallbackThunk<GateTag, bool(const void* self, const void* ray, void* hit, void** gs), /*kCaller=*/true>;

// The slot as a manager target on a libr15 loaded at `base`: it must hold the real function (base + fn).
inline sentinel::GotTarget TargetFor(const char* symbol, const Slot& slot, std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, symbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             slot.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + slot.fn);
  return target.Owned("combat_bvh");
}

}  // namespace nevr_quest::combat::bvh

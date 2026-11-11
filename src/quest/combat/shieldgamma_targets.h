/* The slot and the data layout of the shield colour module (shieldgamma.h), pinned to the store libr15 (build ID in pinned_targets.h).
 *
 *   R58  CSetMaterialPropertyNode::Update   JUMP_SLOT 0x36d2020 -> 0x272d55c   (`readelf -rW`, the slot the q15t plugin hooked)
 *
 * The function is libr15's own, so the slot holds `base + 0x272d55c` in this BIND_NOW image (got_hook.h) and the target says so.
 * got_pinned_test.cpp checks the relocation, the symbol's address and every code word below against the real file
 * (`just test-quest-hooks-pinned`, which needs the store APK).
 *
 * SSetMaterialPropertyNodeData, as Update reads it (x19 is the data pointer): +0x20 material and +0x28 property (`ldp x2, x3, [x19, #0x20]` at
 * 0x272d7f4), +0x38..+0x44 the value r, g, b, a (`ldp s0, s1, [x19, #0x38]` and `ldp s2, s3, [x19, #0x40]` at 0x272d7f8 and 0x272d7fc),
 * +0x4c the colourspace (`ldr w22, [x19, #0x4c]` at 0x272d7e4). +0x30, the value type (1 = vec4), is the reference build's reading and no word
 * below pins it. Update only resolves the actor's model, calls CModelCS::SetMaterialProperty and reads the node's start time (0x272d55c..
 * 0x272d7e0), which is why a second call of it on a copy of the data is the write alone.
 *
 * Origin (docs/audits/2026-11-05-q15t-origin-citations.md, rows 129-132): the reference build's `runtime/native/afkxr/afk_shieldgamma.cpp`:1-12
 * (the colour decode and the material), :15-19 (the layout), :57 (the slot and function), :243-244 (the two switches' defaults).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::shieldgamma {

inline constexpr const char* kSymbol = "_ZN10NRadEngine24CSetMaterialPropertyNode6UpdateERKNS_28SSetMaterialPropertyNodeDataE";
inline constexpr std::uint64_t kSlotVaddr = 0x36d2020ULL;
inline constexpr std::uint64_t kFnVaddr = 0x272d55cULL;

struct PinnedWord {
  const char* what;
  std::uint64_t vaddr;
  std::uint32_t word;
};
// An `ldr w, [x, #imm]` carries imm/4 in bits 10-21; an `ldp x, x, [x, #imm]` carries imm/8 and an `ldp s, s, [x, #imm]` imm/4 in bits 15-21
// (signed 7 bits): so a changed offset or a changed word fails the build.
inline constexpr PinnedWord kWords[] = {
    {"Update: first instruction (str x28, [sp, #-0x40]!)", 0x272d55cULL, 0xf81c0ffcU},
    {"Update: the colourspace at +0x4c (ldr w22, [x19, #0x4c])", 0x272d7e4ULL, 0xb9404e76U},
    {"Update: the material and property at +0x20 (ldp x2, x3, [x19, #0x20])", 0x272d7f4ULL, 0xa9420e62U},
    {"Update: the colour at +0x38 (ldp s0, s1, [x19, #0x38])", 0x272d7f8ULL, 0x2d470660U},
    {"Update: the colour at +0x40 (ldp s2, s3, [x19, #0x40])", 0x272d7fcULL, 0x2d480e62U},
};
constexpr std::uintptr_t Imm12(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff); }
constexpr std::uintptr_t Imm7(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 15) & 0x7f); }

// SSetMaterialPropertyNodeData: +0x20 material, +0x28 property, +0x30 type (1 = vec4), +0x38 value (r, g, b, a), +0x4c colourspace (0 = linear).
// Update reads up to +0x4c; the copy the tube mirror makes is 0x58 bytes.
inline constexpr std::size_t kDataSize = 0x58;
inline constexpr std::uintptr_t kDataMaterial = 0x20, kDataProperty = 0x28, kDataType = 0x30, kDataValue = 0x38, kDataSpace = 0x4c;
static_assert(Imm12(kWords[1].word) * 4 == kDataSpace, "the pinned load of the colourspace does not carry kDataSpace");
static_assert(Imm7(kWords[2].word) * 8 == kDataMaterial && kDataProperty == kDataMaterial + 8, "the pinned load of the material and property does not carry kDataMaterial");
static_assert(Imm7(kWords[3].word) * 4 == kDataValue, "the pinned load of the colour does not carry kDataValue");
static_assert(Imm7(kWords[4].word) * 4 == kDataValue + 8, "the pinned load of the colour's blue and alpha does not carry kDataValue + 8");

inline constexpr std::int32_t kVec4 = 1, kLinear = 0;
inline constexpr std::uint64_t kMaterial = 0xdbde1b05fa8f6200ULL;      // the spawn-shield / barrier material
inline constexpr std::uint64_t kTubeMaterial = 0xb28e940eb21a5798ULL;  // the tubefork window material
inline constexpr std::uint64_t kRingProperty = 0xdd35e419672ee0baULL;  // the script's ring colour
inline constexpr float kGamma = 2.2f;                                   // the PC pixel shader's own decode: pow(saturate(x), 2.2)

struct Tag {};
using Sig = std::uint64_t(void* node, const void* data);
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// The slot as a manager target on a libr15 loaded at `base`: it must hold the game's own function (base + kFnVaddr).
inline sentinel::GotTarget Target(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             kSlotVaddr);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kFnVaddr);
  return target.Owned("combat_shieldgamma");
}

}  // namespace nevr_quest::combat::shieldgamma

/* The slot and the data layout of the wrist HUD module (hudmeter.h), pinned to the store libr15 (build ID in pinned_targets.h).
 *
 *   R57  CSetCanvasElementAttributesNode::Enter   JUMP_SLOT 0x36c6ce8 -> 0x2726eac   (`readelf -rW`, the slot the q15t plugin hooked)
 *
 * The function is libr15's own, so the slot holds `base + 0x2726eac` in this BIND_NOW image (got_hook.h) and the target says so.
 * got_pinned_test.cpp checks the relocation, the symbol's address and every code word below against the real file
 * (`just test-quest-hooks-pinned`, which needs the store APK).
 *
 * SSetCanvasElementAttributesNodeData, as Enter reads it (x19 is the data pointer): +0x20 item (`ldr x2, [x19, #0x20]` at 0x2727130), +0x34
 * size flag (`ldr w8, [x19, #0x34]` at 0x272711c, the size is written only when it is not 0), +0x38 size (x, y) (`add x3, x19, #0x38` at 0x2727134,
 * passed to CCanvasUICS::SetElementSize). +0x18, the canvas component, is the reference build's reading and no word below pins it: the
 * module only logs it.
 *
 * Origin (docs/audits/2026-11-05-q15t-origin-citations.md, rows 75-84): the reference build's `runtime/native/afkxr/afk_hudmeter.cpp`:1-9
 * (the zero-seed trap), :43-48 (the offsets and the buffer), :49 (the switch), :76 (the slot and function), :83 (its eleven pinned code
 * words, of which this module pins the first).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::hudmeter {

inline constexpr const char* kSymbol = "_ZN10NRadEngine31CSetCanvasElementAttributesNode5EnterERKNS_35SSetCanvasElementAttributesNodeDataE";
inline constexpr std::uint64_t kSlotVaddr = 0x36c6ce8ULL;
inline constexpr std::uint64_t kFnVaddr = 0x2726eacULL;

struct PinnedWord {
  const char* what;
  std::uint64_t vaddr;
  std::uint32_t word;
};
// The offsets the module reads, in one place, tied to the pinned words below at compile time: an `ldr x, [x, #imm]` (64-bit) carries imm/8,
// an `ldr w, [x, #imm]` imm/4 and an `add x, x, #imm` the unscaled imm in bits 10-21, so a changed offset or a changed word fails the build.
// The first word is Enter's first instruction, `stp x28, x26, [sp, #-0x50]!`; the reference build pins its first eleven.
inline constexpr PinnedWord kWords[] = {
    {"Enter: first instruction (stp x28, x26, [sp, #-0x50]!)", 0x2726eacULL, 0xa9bb6bfcU},
    {"Enter: the size flag at +0x34 (ldr w8, [x19, #0x34])", 0x272711cULL, 0xb9403668U},
    {"Enter: the item at +0x20 (ldr x2, [x19, #0x20])", 0x2727130ULL, 0xf9401262U},
    {"Enter: the size at +0x38 (add x3, x19, #0x38)", 0x2727134ULL, 0x9100e263U},
};
constexpr std::uintptr_t Imm12(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff); }

inline constexpr std::uint64_t kLobbyComponent = 0xe55af932c88ec864ULL;  // the lobby helmet's canvas component
inline constexpr std::uint64_t kItem = 0x9fd2e8cc0e46df3cULL;            // CSymbol64 of "weapon_meter"
inline constexpr std::uintptr_t kDataComponent = 0x18, kDataItem = 0x20, kDataSizeOn = 0x34, kDataSize = 0x38;
static_assert(Imm12(kWords[1].word) * 4 == kDataSizeOn, "the pinned load of the size flag does not carry kDataSizeOn");
static_assert(Imm12(kWords[2].word) * 8 == kDataItem, "the pinned load of the item does not carry kDataItem");
static_assert(Imm12(kWords[3].word) == kDataSize, "the pinned address of the size does not carry kDataSize");
inline constexpr std::size_t kDataPrefix = 0x40;  // read once per call: everything through the size
inline constexpr float kFullX = 86.0f, kFullY = 88.0f;  // weapon_meter's authored size

struct Tag {};
using Sig = std::uint64_t(void* node, const void* data);
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// The slot as a manager target on a libr15 loaded at `base`: it must hold the game's own function (base + kFnVaddr).
inline sentinel::GotTarget Target(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             kSlotVaddr);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kFnVaddr);
  return target.Owned("combat_hudmeter");
}

}  // namespace nevr_quest::combat::hudmeter

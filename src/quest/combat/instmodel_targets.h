/* The pinned slot and code words of the instanced-model table-step module (instmodel.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID b243509c...); the disassembly's address is the ELF address plus
 * 0x100000. src/quest/tests/got_pinned_test.cpp checks the relocation, the function and both words against the real file.
 *
 *   slot  CGInstancedModelResource::CGInstancedModelResource(CPresenceInit const&)   JUMP_SLOT 0x36c5c68 -> 0x19078dc   (`readelf -rW`)
 *   words the constructor's first instruction (`str x20,[sp,#-0x20]!`) and the one growth-step constant (`mov w20,#0x20` at 0x1907958) that
 *         every later store of a step reads: `str x20,[x19,#0x98]` and the seven after it (0xd0, 0x108, 0x140, 0x178, 0x1b0, 0x1e8, 0x220)
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/table_step.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::instmodel {

inline constexpr const char* kSymbol = "_ZN10NRadEngine24CGInstancedModelResourceC2ERKNS_13CPresenceInitE";

struct Pins {
  std::uint64_t ctor;  // CGInstancedModelResource::CGInstancedModelResource(CPresenceInit const&)
  std::uint64_t slot;  // the JUMP_SLOT its callers' PLT stub (0xfdcb00) loads it from
  std::uint64_t stepWord;  // `mov w20,#0x20`
};
inline constexpr Pins kPins = {0x19078dc, 0x36c5c68, 0x1907958};

inline constexpr std::uint32_t kStockCtorFirstWord = 0xf81e0ff4;  // str x20,[sp,#-0x20]!
inline constexpr std::uint32_t kStockStepWord = 0x52800414;       // mov w20,#0x20

inline constexpr table_step::PinWord kWords[] = {
    {kPins.ctor, kStockCtorFirstWord},
    {kPins.stepWord, kStockStepWord},
};
inline constexpr std::size_t kWordCount = sizeof kWords / sizeof kWords[0];

using Sig = void(void* self, const void* init);
struct Tag {};
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// The slot as a manager target on a libr15 loaded at `base`: the slot must hold the constructor itself (base + ctor).
inline sentinel::GotTarget Target(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             pins.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.ctor);
  return target.Owned("combat_instmodel");
}

}  // namespace nevr_quest::combat::instmodel

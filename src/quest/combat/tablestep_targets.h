/* The pinned slot and code word of the shader-set factory's CreateInstance module (tablestep.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID b243509c...); the disassembly's address is the ELF address plus
 * 0x100000. src/quest/tests/got_pinned_test.cpp checks the relocation, the function and the word against the real file.
 *
 *   slot  CFactory<CGShaderSetResource>::CreateInstance(CPresenceInit*)   ABS64 0x365ddd0 -> 0x190707c   (`readelf -rW`)
 *   word  `mov w8,#0x20` at 0x1907130 (the growth step; `str x8,[x19,#0x98]` follows at 0x1907138)
 *
 * The slot is vtable slot 2 of the same factory vtable as shaderstep_targets.h (slot 1), a class C word of docs/adr/0046. It is the only
 * dynamic relocation of the store libr15 whose target is CreateInstance; whether every caller reaches the function through it is not shown.
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

namespace nevr_quest::combat::tablestep {

inline constexpr const char* kSymbol = "_ZN10NRadEngine8CFactoryINS_19CGShaderSetResourceEE14CreateInstanceEPNS_13CPresenceInitE";

struct Pins {
  std::uint64_t fn;    // CFactory<CGShaderSetResource>::CreateInstance
  std::uint64_t slot;  // the vtable word that holds it
  std::uint64_t stepWord;
};
inline constexpr Pins kPins = {0x190707c, 0x365ddd0, 0x1907130};

inline constexpr std::uint32_t kStockStepWord = 0x52800408;  // mov w8,#0x20

inline constexpr table_step::PinWord kWords[] = {{kPins.stepWord, kStockStepWord}};
inline constexpr std::size_t kWordCount = sizeof kWords / sizeof kWords[0];

using Sig = void*(void* unused, void* init);
struct Tag {};
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// The slot as a manager target on a libr15 loaded at `base`: the word must hold the function itself (base + fn).
inline sentinel::GotTarget Target(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId(), pins.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.fn);
  return target.Owned("combat_tablestep");
}

}  // namespace nevr_quest::combat::tablestep

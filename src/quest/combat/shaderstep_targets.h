/* The pinned slot of the shader-set factory's ConstructInstance module (shaderstep.h).
 *
 * The address is an ELF virtual address in the store libr15 (build ID b243509c...); the disassembly's address is the ELF address plus 0x100000.
 * src/quest/tests/got_pinned_test.cpp checks the relocation (ABS64, one word names the symbol) and the function against the real file.
 *
 *   slot  CFactory<CGShaderSetResource>::ConstructInstance(void*, CPresenceInit*)   ABS64 0x365ddc8 -> 0x1906f4c   (`readelf -rW`)
 *
 * The slot is vtable slot 1 of the factory's vtable, whose address point is `_ZTV + 0x10` (the factory's destructor, 0x1906ef0, stores it);
 * it is a class C word of docs/adr/0046 (a vtable word a relocation names), so the target pins it and the manager writes it as a hook.
 * The function stores the table's growth step on its common tail: `mov w8,#0x20` at 0x1907000, `str x8,[x19,#0x98]` at 0x1907004.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::shaderstep {

inline constexpr const char* kSymbol =
    "_ZN10NRadEngine8CFactoryINS_19CGShaderSetResourceEE17ConstructInstanceEPvPNS_13CPresenceInitE";

struct Pins {
  std::uint64_t fn;    // CFactory<CGShaderSetResource>::ConstructInstance
  std::uint64_t slot;  // the vtable word that holds it
};
inline constexpr Pins kPins = {0x1906f4c, 0x365ddc8};

using Sig = void*(void* self, void* obj, const void* init);
struct Tag {};
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// The slot as a manager target on a libr15 loaded at `base`: the word must hold the function itself (base + fn).
inline sentinel::GotTarget Target(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId(), pins.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.fn);
  return target.Owned("combat_shaderstep");
}

}  // namespace nevr_quest::combat::shaderstep

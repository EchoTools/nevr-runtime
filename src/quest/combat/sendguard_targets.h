/* The game words sendguard reads and hooks (#2179), as ELF vaddrs of the store libr15 (the disassembly lists each 0x100000 higher).
 * got_pinned_test.cpp checks every line below against the store file: the relocation, the symbol's definition and each code word.
 *
 * CSendEventNode::Enter is defined in libr15 at 0x2725a80 (476 bytes, `.dynsym` FUNC). It is called through one JUMP_SLOT, 0x36f7928
 * (`readelf -rW`: R_AARCH64_JUMP_SLOT, symbol value 0x2725a80). That is the one word the module hooks.
 *
 * What the file also holds at 0x2725a80, and is not a slot. The disassembly's data cross-references to Enter list three words: the
 * JUMP_SLOT above, one at 0x356a6b0 and one at 0x33e7924. The first of those is the eight bytes `d0b31bff dc010000` inside `.eh_frame`
 * (0x330ed50 to 0x35f7e10): an FDE's pc_begin (0x356a6b0 + the int32 0xff1bb3d0 = 0x2725a80) and its range 0x1dc, Enter's own size, so it is
 * the function's unwind record and no relocation names it (`readelf -rW` lists none at 0x356a6b0). The second is 4-aligned and cannot
 * hold a pointer word. Neither is a hook target, and the module touches neither.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::sendguard {

inline constexpr const char* kEnterSymbol = "_ZN10NRadEngine14CSendEventNode5EnterERKNS_18SSendEventNodeDataE";
inline constexpr std::uint64_t kEnterVaddr = 0x2725a80ULL;
inline constexpr std::uint64_t kEnterSlotVaddr = 0x36f7928ULL;

// void CSendEventNode::Enter(execution context x0, const SSendEventNodeData& x1). data[0] is the level key (-1: the current space),
// data[1] the event key, data[2] the actor key.
using EnterSig = void(void* context, const void* data);
struct EnterTag {};
using EnterThunk = sentinel::CallbackThunk<EnterTag, EnterSig>;

inline sentinel::GotTarget EnterTarget() {
  return {sentinel::pinned::kLibR15, kEnterSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), kEnterSlotVaddr};
}

// CncaGameSpace::GameSpaceMT(space, level key), the lookup the stock function makes: libr15 defines it at 0x1806704 and calls it through
// the PLT stub at 0xef5650, whose four words are pinned (the stub loads its JUMP_SLOT, 0x36d2210, and branches to it). The module calls
// the stub, as Enter does, so a lazy bind would be resolved by the loader.
inline constexpr std::uint64_t kGameSpaceMtStubVaddr = 0xef5650ULL;
using GameSpaceMtSig = void*(void* space, std::uint64_t levelKey);

// The words the wrapper's reads and the fault were derived from, read from the loaded library before the slot is touched. Enter reads
// the current space through ctx -> [0] -> [0] -> [+0x30] -> [0] -> [+0x80] (x8, x9 and x19 at 0x2725a90 to 0x2725ab0), loads the level key
// (0x2725a98 `ldr x1,[x1]`), asks GameSpaceMT (0x2725abc) and, with a null answer, reads `[x19 + 0x58]` at 0x2725b78: the load that faults.
// A pin is one word, so each reads as its own row.
inline constexpr sentinel::pinned::PinnedCode kCode[] = {
    {"Enter: sub sp,sp,#0x70", 0x2725a80ULL, {0xd101c3ff, 0, 0, 0, 0, 0}, 1},
    {"Enter: ldr x8,[x0]", 0x2725a90ULL, {0xf9400008, 0, 0, 0, 0, 0}, 1},
    {"Enter: ldr x1,[x1] (the level key)", 0x2725a98ULL, {0xf9400021, 0, 0, 0, 0, 0}, 1},
    {"Enter: ldr x9,[x8]", 0x2725aa0ULL, {0xf9400109, 0, 0, 0, 0, 0}, 1},
    {"Enter: ldr x9,[x9,#0x30]", 0x2725aa8ULL, {0xf9401929, 0, 0, 0, 0, 0}, 1},
    {"Enter: ldr x9,[x9]", 0x2725aacULL, {0xf9400129, 0, 0, 0, 0, 0}, 1},
    {"Enter: ldr x19,[x9,#0x80] (the current space)", 0x2725ab0ULL, {0xf9404133, 0, 0, 0, 0, 0}, 1},
    {"Enter: bl GameSpaceMT", 0x2725abcULL, {0x979f3ee5, 0, 0, 0, 0, 0}, 1},
    {"Enter: ldr x9,[x19,#0x58] (the faulting load)", 0x2725b78ULL, {0xf9402e69, 0, 0, 0, 0, 0}, 1},
    {"GameSpaceMT stub", 0xef5650ULL, {0xb0013ef0, 0xf9410a11, 0x91084210, 0xd61f0220, 0, 0}, 4},
};
inline constexpr std::size_t kCodeCount = sizeof(kCode) / sizeof(kCode[0]);

}  // namespace nevr_quest::combat::sendguard

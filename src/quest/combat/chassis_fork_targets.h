/* The pinned sites of the chassis-fork module (#1649, #1669, #1644; the module itself is not in the tree yet): the model swapper's BeginModelSwap, the resource
 * manager's LookupResourcePath, the resource manager's word in .bss and the store that writes it. Behaviour is in the upstream
 * reference (kotorvr/echoquest-combat at c5a67e8, runtime/native/afkxr/afk_fx.cpp, RE-52 fxchassisfork around lines 7529-7560).
 *
 * Every address is an ELF virtual address in the store libr15 (its build ID is kLibR15StockBuildId in pinned_targets.h). The
 * disassembly's address is the ELF address plus 0x100000 (ReVault project echovr, binary libr15.so).
 *
 *   NRadEngine::CModelSwapperCS::BeginModelSwap(SCompactPoolHandle, CSymbol64)  size 724, function 0x1634f7c
 *     JUMP_SLOT   0x3700470 -> 0x1634f7c   (the slot upstream's wrapper is installed on)
 *     ABS64       0x364c030 -> 0x1634f7c   (a second route to BeginModelSwap: the vtable word; ReVault labels it vslot[96]).
 *                                          A call through this word does not go through the JUMP_SLOT, so a hook on the
 *                                          JUMP_SLOT does not see it. This header makes no decision about that route.
 *     prologue    0x1634f7c 0xd10183ff, 0x1634f80 0xa9016bfb, 0x1634f84 0xa90263f9 (read from the file)
 *   NRadEngine::CResourceManager::LookupResourcePath(CSymbol64, CSymbol64, unsigned long long, CFixedStringT<512>&) const
 *                                              size 532, function 0x2544228
 *     JUMP_SLOT   0x36cd0e0 -> 0x2544228
 *   The resource manager word: 0x376ed60 (a runtime global). It lies in .bss (the file's section header; no file bytes), so its
 *   value is not pinned, only its address and the instructions that write it:
 *     0x2aa89b0  0xd0006628  adrp x8, 0x376e000   (page of the word; x8 is the base of the store)
 *     0x2aa89b4  0xf9001734  str x20, [x25, #0x28] (writes no x8)
 *     0x2aa89b8  0xb4000054  cbz x20, 0x2aa89c0   (writes no x8)
 *     0x2aa89bc  0xf906b114  str x20, [x8, #0xd60] (the store: 0x376e000 + 0xd60 = 0x376ed60)
 *   All four are in CEngine::InitGlobals (ReVault VA 0x2ba87ec, size 2664). A second store writes the same word, clearing it with
 *   `str xzr, [x21, #0xd60]` at 0x2aac4c0 (ReVault VA 0x2bac4c0); it is not pinned here.
 *
 * Upstream's tables (kotorvr/echoquest-combat at c5a67e8, afk_fx.cpp): 32 combat pairs, `kChFork[32]` at line 7553, which the
 * lobby combat wing shares (loops at 7792 and 7876), and 32 social pairs, `kChForkSoc[32]` at line 7592. No separate lobby table.
 *
 * src/quest/tests/chassis_fork_pinned_test.cpp checks every site against the real file (`just test-quest-hooks-pinned`).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::chassis_fork::targets {

inline constexpr const char* kSwapSymbol = "_ZN10NRadEngine15CModelSwapperCS14BeginModelSwapENS_18SCompactPoolHandleENS_9CSymbol64E";
inline constexpr const char* kLookupSymbol =
    "_ZNK10NRadEngine16CResourceManager18LookupResourcePathENS_9CSymbol64ES1_yRNS_13CFixedStringTILy512EEE";

// The slots, and the function each symbol resolves to, in the store libr15.
struct Pins {
  std::uint64_t swapSlot, swapFn;      // JUMP_SLOT
  std::uint64_t swapVtableSlot;        // ABS64, the same function
  std::uint64_t lookupSlot, lookupFn;  // JUMP_SLOT
};
inline constexpr Pins kPins = {0x3700470ULL, 0x1634f7cULL, 0x364c030ULL, 0x36cd0e0ULL, 0x2544228ULL};

// The first three words of BeginModelSwap as the file holds them.
inline constexpr std::size_t kSwapPrologueCount = 3;
inline constexpr pinned::Word kSwapPrologue[kSwapPrologueCount] = {
    {"BeginModelSwap: sub sp,sp,#0x60", 0x1634f7cULL, 0xd10183ffU},
    {"BeginModelSwap: stp x27,x26,[sp,#0x10]", 0x1634f80ULL, 0xa9016bfbU},
    {"BeginModelSwap: stp x25,x24,[sp,#0x20]", 0x1634f84ULL, 0xa90263f9U},
};

// The resource manager word, its section, and the four words that set x8 and store through it (adrp, two instructions that write
// no x8, the store). The section is named by the file's section header; the word's value is not pinned (it is zero in the file).
inline constexpr std::uint64_t kResMgrWord = 0x376ed60ULL;
inline constexpr const char* kResMgrSection = ".bss";
inline constexpr std::uint64_t kResMgrWriterAt = 0x2aa89b0ULL;  // the first of the four words
inline constexpr std::size_t kResMgrWriterCount = 4;
inline constexpr pinned::Word kResMgrWriter[kResMgrWriterCount] = {
    {"InitGlobals: adrp x8,0x376e000", 0x2aa89b0ULL, 0xd0006628U},
    {"InitGlobals: str x20,[x25,#0x28] (no x8)", 0x2aa89b4ULL, 0xf9001734U},
    {"InitGlobals: cbz x20,0x2aa89c0 (no x8)", 0x2aa89b8ULL, 0xb4000054U},
    {"InitGlobals: str x20,[x8,#0xd60] (the resource manager word)", 0x2aa89bcULL, 0xf906b114U},
};

// The two JUMP_SLOT and ABS64 targets, each with its slot pinned. A target carries no expected original: the slot's value at run
// time is the install's check, not this header's.
inline sentinel::GotTarget SwapTarget(const Pins& pins = kPins) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, kSwapSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.swapSlot);
}
inline sentinel::GotTarget SwapVtableTarget(const Pins& pins = kPins) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, kSwapSymbol, sentinel::RelocKind::kAbs64,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.swapVtableSlot);
}
inline sentinel::GotTarget LookupTarget(const Pins& pins = kPins) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, kLookupSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.lookupSlot);
}

}  // namespace nevr_quest::combat::chassis_fork::targets

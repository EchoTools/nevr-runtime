/* The pinned slots of the outline-mode module (#1640 and #1656; the module itself is not in the tree yet): the two SetOutlineMode JUMP_SLOTs the module
 * chains, the function each one holds, and the symbol each one names. Behaviour is in the upstream reference (kotorvr/echoquest-combat
 * at c5a67e8, runtime/native/afkxr/afk_fx.cpp: fxoutlinefix2 and olnoteam, around lines 5267-5345 and 8152-8202).
 *
 * Every address is an ELF virtual address in the store libr15 (its build ID is kLibR15StockBuildId in pinned_targets.h). The
 * disassembly's address is the ELF address plus 0x100000 (ReVault project echovr, binary libr15.so).
 *
 *   CGScene::SetOutlineMode(EMeshOutlineMode)                                          JUMP_SLOT 0x36fb718 -> 0x18c5140  (size 236)
 *   CGDrawDataSet::SetOutlineMode(EMeshOutlineMode, CGVertexFormat const&, EPrimType)  JUMP_SLOT 0x36e3bc0 -> 0x183ab9c  (size 292)
 *
 * CGScene::SetOutlineMode calls CGDrawDataSet::SetOutlineMode for the draw data sets of each mesh it walks (ReVault: a direct call
 * at 0x19c51fc to the thunk at 0x10189b0). Each function in the table above is the .dynsym value of the symbol the slot's
 * relocation names. src/quest/tests/outline_mode_pinned_test.cpp checks each slot's relocation, the symbol, and the function address
 * against the real file (`just test-quest-hooks-pinned`).
 */
#pragma once

#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::outline_mode::targets {

inline constexpr const char* kSceneSymbol = "_ZN10NRadEngine7CGScene14SetOutlineModeENS_16EMeshOutlineModeE";
inline constexpr const char* kDrawSetSymbol =
    "_ZN10NRadEngine13CGDrawDataSet14SetOutlineModeENS_16EMeshOutlineModeERKNS_14CGVertexFormatENS_9EPrimTypeE";

// The slots, and the function each slot's symbol resolves to in the store libr15.
struct Pins {
  std::uint64_t sceneSlot, sceneFn;
  std::uint64_t drawSetSlot, drawSetFn;
};
inline constexpr Pins kPins = {0x36fb718ULL, 0x18c5140ULL, 0x36e3bc0ULL, 0x183ab9cULL};

// The two JUMP_SLOT targets, each with its slot pinned. A target carries no expected original: the slot's value at run time is
// the install's check, not this header's.
inline sentinel::GotTarget SceneTarget(const Pins& pins = kPins) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, kSceneSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.sceneSlot);
}
inline sentinel::GotTarget DrawSetTarget(const Pins& pins = kPins) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, kDrawSetSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.drawSetSlot);
}

}  // namespace nevr_quest::combat::outline_mode::targets

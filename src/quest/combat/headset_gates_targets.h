/* The pinned slots, functions and code words of the headset gates (headset_gates.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address
 * is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks both JUMP_SLOT relocations and every code word against the real
 * file (`just test-quest-hooks-pinned`, which needs the store APK).
 *
 *   PartyMemberHeadsetType  JUMP_SLOT 0x36fd880 -> 0x129b168   (`readelf -rW`; the one caller that matters is the permissions expression)
 *   CVR::HeadsetType        JUMP_SLOT 0x36f9fd8 -> 0x1a6f164   (`mov w0,#3; ret`: this build always answers 3, the Quest kind)
 *
 * The three callers the gates answer for, each a range of the game's own code that the return address lies in:
 *   perm  CR15NetPartyPermissionsExpression::operator() [0x232c794, 0x232c854): its `bl PartyMemberHeadsetType` at 0x232c810 returns to
 *         0x232c814, where `cmp w0,#3; b.eq` clears the combat permission flag (+1) of a party member whose headset kind is 3.
 *   bots  CR15NetGame::BotActorId(CSymbol64) [0x124dd38, 0x124e004): its `bl CVR::HeadsetType` at 0x124dd40 returns to 0x124dd44, where
 *         `cmp w0,#3; b.ne` sends a Quest to bot body 0, the arena bot.
 *   link  BindExpression<CR15HeadsetTypeExpression> [0x202ba74, 0x202bac8): its `bl CVR::HeadsetType` at 0x202baa0 returns to 0x202baa4, and the
 *         kind (0..4) becomes the four flags the menu scripts read. The Bind opens `sub sp,sp,#0x20; stp x19,x30,[sp,#0x10]` and calls
 *         from that frame, so its caller's address is at its entry sp + 0x18.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::headset_gates {

inline constexpr const char* kPartySymbol = "_ZNK10NRadEngine8NRadGame11CR15NetGame22PartyMemberHeadsetTypeEj";
inline constexpr const char* kHeadsetSymbol = "_ZN10NRadEngine3CVR11HeadsetTypeEv";

// The slots, the real functions and the caller ranges (ELF vaddrs; a range is [lo, hi)).
struct Pins {
  std::uint64_t partySlot, partyFn;
  std::uint64_t headsetSlot, headsetFn;
  std::uint64_t permLo, permHi;
  std::uint64_t botsLo, botsHi;
  std::uint64_t linkLo, linkHi;
  std::uint64_t levelNameSlot, levelNameStock;  // CR15Game's LoadLevelName vtable word, and the stock function it holds (the bot gate's condition)
};
inline constexpr Pins kPins = {0x36fd880ULL, 0x129b168ULL, 0x36f9fd8ULL, 0x1a6f164ULL, 0x232c794ULL,
                               0x232c854ULL, 0x124dd38ULL,  0x124e004ULL, 0x202ba74ULL, 0x202bac8ULL,
                               0x362ef90ULL, 0x1202eccULL};

// levelname's own pins (src/quest/plugins/q15t/src/levelname.cpp kSlots[0], kFnVaddr; got_pinned_test.cpp checks the slot's relocation names
// CR15Game::LoadLevelName): the word is the stock function until levelname's hook replaces it.

// The Bind's saved return address, from its entry sp: `stp x19,x30,[sp,#0x10]` after `sub sp,sp,#0x20` (the word pinned at 0x202ba80).
inline constexpr std::uintptr_t kBindSavedLrOffset = 0x18;

enum class Gate : int { kPerm = 0, kBots = 1, kLink = 2 };

// One pinned instruction word and the gate refused when it differs.
struct Word {
  std::uint64_t vaddr;
  std::uint32_t word;
  Gate gate;
};
inline constexpr std::size_t kWordCount = 13;
inline constexpr Word kWords[kWordCount] = {
    {0x232c794, 0xd10103ff, Gate::kPerm},  // operator(): sub sp,sp,#0x40
    {0x232c810, 0x97b07ec8, Gate::kPerm},  // bl PartyMemberHeadsetType
    {0x232c814, 0x71000c1f, Gate::kPerm},  // cmp w0,#3  (the return address of that call)
    {0x232c818, 0x54fffe00, Gate::kPerm},  // b.eq: the branch that clears the combat flag for a Quest
    {0x124dd38, 0xa9bf7bf3, Gate::kBots},  // BotActorId: stp x19,x30,[sp,#-0x10]!
    {0x124dd40, 0x97f3dd28, Gate::kBots},  // bl CVR::HeadsetType
    {0x124dd44, 0x71000c1f, Gate::kBots},  // cmp w0,#3  (the return address of that call)
    {0x202ba74, 0xb4000341, Gate::kLink},  // Bind: cbz x1
    {0x202ba7c, 0xd10083ff, Gate::kLink},  // sub sp,sp,#0x20
    {0x202ba80, 0xa9017bf3, Gate::kLink},  // stp x19,x30,[sp,#0x10]  (the caller's address is at sp+0x18)
    {0x202baa0, 0x97bc65d0, Gate::kLink},  // bl CVR::HeadsetType
    {0x202baa4, 0x7100101f, Gate::kLink},  // cmp w0,#4  (the return address of that call)
    {0x202bac8, 0x52800028, Gate::kLink},  // mov w8,#1: the first instruction after the range
};

// The slot as a manager target on a libr15 loaded at `base`: the slot must hold the real function (base + fn) or the chain's oldest original.
inline sentinel::GotTarget PartyTarget(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kPartySymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.partySlot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.partyFn);
  return target.Owned("combat_gates");
}
inline sentinel::GotTarget HeadsetTarget(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kHeadsetSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.headsetSlot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.headsetFn);
  return target.Owned("combat_gates");
}

}  // namespace nevr_quest::combat::headset_gates

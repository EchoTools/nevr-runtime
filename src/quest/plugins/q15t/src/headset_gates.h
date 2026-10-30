// Return-address-gated wrappers on the game's headset-type imports.
//
// The game decides "this is a Quest headset" with two imported calls (CVR::HeadsetType and
// CR15NetGame::PartyMemberHeadsetType). Two functions of the game branch on that answer in a way a Quest build has to skip:
// the party-permissions expression and the bot-body chooser. A wrapper returns the real answer to every caller except a call
// that returns into one of those two functions, where it answers "not a Quest headset" (any value other than 3, which is
// all those functions compare with). That is the effect of removing the branch, without writing a code page: the wrapper
// reads its own return address, which is data.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

struct CallerRange {
  std::uintptr_t lo_vaddr;  // ELF vaddr of the function's first byte
  std::uintptr_t hi_vaddr;  // one past its last byte
};

inline constexpr std::int32_t kLinkHeadset = 4;  // the PC build's own kind for a Quest on Link: the Bind sets only its flag3, never the Quest flag (kind 3)

// `real_vaddr`: ELF vaddr of the real function in the pinned build (the slot's symbol value).
// PartyMemberHeadsetType(this, index): answers `kNotQuest` to a call from `range`.
InstallResult InstallPartyHeadsetGate(std::uintptr_t base, std::uintptr_t slot_vaddr, CallerRange range, std::uintptr_t real_vaddr);
// CVR::HeadsetType(): answers `kNotQuest` to a call from `range`, and `link_answer` to a call from `link_range`
// (an empty range, lo == hi, gates nothing). The second range is the headset-type expression's Bind: it turns the
// answer into four flags (0,1 -> rift; 2 -> rifts; 3 -> quest; 4 -> link; anything above 4 -> none), which the menu scripts read.
// The link answer must be one of those kinds: an answer above 4 leaves all four flags clear, and a script that picks its target
// by headset kind then gets none. `kLinkHeadset` sets only the link flag, so no menu script sees a Quest.
InstallResult InstallHeadsetGate(std::uintptr_t base, std::uintptr_t slot_vaddr, CallerRange range, std::uintptr_t real_vaddr,
                                 CallerRange link_range = {0, 0}, std::int32_t link_answer = kLinkHeadset);

// What the install configured (absolute addresses), for the wiring test: the production constants cannot be exercised by a call on the host.
struct HeadsetGateRanges {
  std::uintptr_t bots_lo, bots_hi, link_lo, link_hi;
  std::int32_t link_answer;
};
HeadsetGateRanges GetHeadsetGateRangesForTest();

struct GateStats {
  std::uint32_t party_calls, party_gated, headset_calls, headset_gated, link_gated;
};
GateStats GetGateStats();

inline constexpr std::int32_t kNotQuest = 0;

}  // namespace q15t

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

// `real_vaddr`: ELF vaddr of the real function in the pinned build (the slot's symbol value).
// PartyMemberHeadsetType(this, index): answers `kNotQuest` to a call from `range`.
InstallResult InstallPartyHeadsetGate(std::uintptr_t base, std::uintptr_t slot_vaddr, CallerRange range, std::uintptr_t real_vaddr);
// CVR::HeadsetType(): answers `kNotQuest` to a call from `range`, and `link_answer` to a call from `link_range`
// (an empty range, lo == hi, gates nothing). The second range is the headset-type expression's Bind: it turns the
// answer into four flags (0,1 -> rift; 2 -> rifts; 3 -> quest; 4 -> link; anything above 4 -> none), which the menu scripts read.
// An answer above 4 leaves all four flags clear: no menu script sees a Quest headset, and none sees another kind either.
InstallResult InstallHeadsetGate(std::uintptr_t base, std::uintptr_t slot_vaddr, CallerRange range, std::uintptr_t real_vaddr,
                                 CallerRange link_range = {0, 0}, std::int32_t link_answer = 5);

// Which script libraries the link gate answers. The expression's Bind is called from the code of the script library that holds the
// node (lib<hash>.so): the wrapper reads the Bind's saved return address off its stack frame, maps it to that library with
// dladdr and compares the library's file name with this policy. `spec`: "all" (every caller from the link range: the default
// until this is called), "none" or empty (no caller: the real answer everywhere), or space/comma separated name fragments
// ("0e4384a5 1703a89b"): a library whose file name contains one is answered, every other library gets the real answer. A library
// that cannot be named counts as not in the list. Call before InstallHeadsetGate. On the host (no frame) a call from the link
// range is a library that cannot be named.
void SetLinkGateLibs(const char* spec);
// How many distinct libraries have bound the expression from the link range (each logged one `headset bind` line).
std::uint32_t LinkGateLibsSeen();

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
inline constexpr std::int32_t kNoHeadsetFlags = 5;  // the Bind's switch has no case above 4: every flag stays clear

}  // namespace q15t

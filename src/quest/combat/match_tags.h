/* Combat name tags (MatchTagDisplay) are on by default (#2090, #1737 unit U24).
 *
 * What the game does (store libr15; addresses are the ELF vaddr, pinned_targets.h). The CR15Game constructor (C1 and C2 are one
 * function, 0x11fce34) stores the default SR15GameSettings into the game object twice, from two 16-byte literals in .rodata:
 *
 *   0x2b9fe40  {0, 0x1003, 1, 0}      stored at this+0x8c04, so the flags word is this+0x8c08   (the current settings)
 *   0x2b9fe60  {0x1003, 1, 0, 0xa}    stored at this+0x8ce4, so the flags word is this+0x8ce4   (the pending settings)
 *
 * Bit 13 (0x2000) of that word is MatchTagDisplay: SR15GameSettings::Inspect<CJsonInspectorRead> (0x20e80c8) passes the string
 * "MatchTagDisplay" (.rodata 0x2ba786e) with the mask 0x2000 to InspectFlagBit, and the script binding
 * (BindExpression<CR15GameSettingsExpression>, 0x201f8dc) hands scripts `(flags >> 13) & 1`. CR15Game::ApplyGameSettingsChanges
 * compares the current copy with the pending one, so the two are changed together.
 *
 * Where a saved choice comes in. CR15Game::LoadSettings (0x11f3e88) runs after the constructor: with a settings file it reads
 * it into this+0x8c08 (SR15GameSettings::FromJson, whose InspectFlagBit sets or clears a bit only for a key the file holds and
 * leaves it alone otherwise) and then copies the current settings over the pending ones; with no settings file it returns before
 * touching either. So a file that holds MatchTagDisplay (a player's saved false included) overrides this default, and a file or a
 * start without the key keeps it.
 *
 * Who that reaches. Every stock save writes the key: SR15GameSettings::Inspect<CJsonInspectorWrite> (0x20ed668) passes "MatchTagDisplay"
 * and the mask 0x2000 to CJsonInspectorWrite::InspectFlagBit (0x1414264), which stores `tst w8, w21; cset w2, ne` as a boolean, so a
 * settings file that was ever saved holds an explicit true or false. The default therefore reaches a player with no settings file, or
 * with a file that has no such key; a player who has saved once keeps what the file says. The reference build changes the same
 * default and has the same reach. No migration of an existing file is made here.
 *
 * What this does. The reference combat build rewrites the two literals, 0x1003 to 0x3003. Here the constructor is wrapped
 * instead: the original runs, then bit 13 is set in each copy that still holds exactly the stock default 0x1003, so a copy
 * something else already changed (it holds no stock value) is left alone. One line per construction says what each copy held
 * before and after, whether or not anything changed.
 *
 * Which slots. Both constructor entry points are JUMP_SLOT imports of libr15's own symbols (pinned_targets.h): C1 (slot 0x36c0018,
 * called by Main_ for the game itself) and C2 (slot 0x36e7828, called by CR15NetDedicatedServer, CR15NetServer and CR15NetLanServer
 * for the base object). Both are hooked, so every construction path gets the default whichever symbol it takes; no ADR 0021 row is
 * needed, because libr15 is linked BIND_NOW and calls the constructor through a PLT stub.
 *
 * Which APKs (docs/adr/0046 rule 5). This ships to every APK, the release APK and the variants alike, and is not gated by combat::Active()
 * (combat_active.h): it installs on every launch (entry.cpp, InstallPermissionWaitHook) whatever the variant stamp says.
 *
 * Frequency (ADR 0038). The constructor runs once per process (Main_), so the wrapper's cost is paid once; it is not a hot path.
 *
 * Owner (ADR 0039). This file owns the default of the MatchTagDisplay bit at construction. A player's choice is the game's own
 * saved settings, which the game applies after this.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_match_tags_install","result":"installed|partial|refused","c1":<GotStatus>,"c2":<GotStatus>}
 *   default  {"event":"combat_match_tags_default","ctor":"c1|c2","result":"set|kept","changed":<copies changed, 0 to 2>,
 *             "current_before":"0x%08x","current_after":"0x%08x","pending_before":"0x%08x","pending_after":"0x%08x"}
 *            written after the original returns, never while game code is on the stack above the handler (callback_thunk.h rule 1).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::match_tags {

using C1Thunk = sentinel::pinned::LibR15CR15GameC1Thunk;
using C2Thunk = sentinel::pinned::LibR15CR15GameC2Thunk;

// Byte offsets of the flags word in the CR15Game object: the current SR15GameSettings and the pending copy (the object is not
// otherwise described here; the offsets are the ones the constructor stores the literals at, +4 for the current copy, whose
// 16-byte literal starts with a zero word).
inline constexpr std::size_t kCurrentFlagsOffset = 0x8c08;
inline constexpr std::size_t kPendingFlagsOffset = 0x8ce4;

inline constexpr std::uint32_t kStockFlags = 0x1003;          // what both literals hold
inline constexpr std::uint32_t kMatchTagDisplayBit = 0x2000;  // bit 13

// The flags a copy that held `flags` after the constructor holds after this: the stock default with bit 13 set, anything else as it was.
constexpr std::uint32_t DefaultedFlags(std::uint32_t flags) noexcept {
  return flags == kStockFlags ? (flags | kMatchTagDisplayBit) : flags;
}

// The `result` of the install line: "installed" when both slots took their thunk, "partial" when one did (the other construction
// path stays the game's own), "refused" when neither did.
const char* InstallResultName(bool c1Installed, bool c2Installed) noexcept;

// Installs the hooks into libr15's C1 and C2 constructor slots. True when both slots hold their thunks. Logs one
// combat_match_tags_install line; a refused slot leaves the game's constructor call as it is.
bool Install() noexcept;

// What an install does with the two slots' statuses once both installs were tried: a slot that did not take its thunk gets its
// handler disarmed, and one combat_match_tags_install line says how many did. True when both are kOk.
bool SettleInstall(sentinel::GotStatus c1, sentinel::GotStatus c2) noexcept;

// The install on any two targets (the host test installs it against modules that are not loaded).
bool InstallAt(const sentinel::GotTarget& c1, const sentinel::GotTarget& c2) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Arms both handlers without touching a GOT slot.
void ArmForTest() noexcept;
// Disarms both handlers and clears the thunks' state.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::match_tags

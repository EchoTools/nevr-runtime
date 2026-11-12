/* global_mp keeps its plain level name on a Quest (#2181, docs/quest/combat/levelname.md): the full-size global level loads in place of the cut one.
 *
 * What the game does. CR15Game::LoadLevelName (store libr15 0x1202ecc, pinned in levelname_targets.h) answers the name `CncaGameSpace::LoadLevel`
 * loads for a requested level. When bit 31 of the word at game+0x7ad8 is set (a Quest's "lowspec" flag) it appends "_lowspec" to the name, so
 * global_mp (CSymbol64 0x3F9915D3001DC28E) loads as Ready At Dawn's cut Quest level; with the bit clear it answers the plain name. The combat
 * bot body exists only in the full-size global_mp, which the combat pack carries (levelname_targets.h; docs/quest/combat/levelname.md).
 *
 * What this does. The function is reached only through virtual slot +0x260 of the four vtables of the CR15Game class tree, each an
 * R_AARCH64_ABS64 word holding the function. Each of the four words gets its own thunk (callback_thunk.h) through the hook manager
 * (`InstallThunk`, ADR 0046 rule 2): for global_mp with the lowspec bit set the handler answers what the plain function answers, the name; for
 * any other name or with the bit clear it calls the function the word held, with the same arguments. All four words or none: a word the manager
 * refuses takes the ones already installed out again, and the line says which word and why.
 *
 * Arena (same APK). The level is the game's global level, loaded once after login, before any lobby: nothing says yet whether the player is
 * going to an arena or a combat match, and the loaded-level answer (combat_level.h) is "none" at that moment, so this module cannot be scoped by
 * level. On a combat APK an arena match runs with the full-size global level too, when `combat.levelname` is on (the default). The
 * handler does nothing for any other level, and a lowspec-clear game is not touched. See docs/quest/combat/levelname.md ("What an arena match
 * gets") for the cost and what is not measured. `combat.levelname: false` leaves the game's cut level (and, with it, boosterpool.h and the bot-body
 * gate, which needs the full level: headset_gates.h "The bot gate" reads this module's slot, the CR15Game word, and acts only where it is not the stock function).
 *
 * Which APKs. The combat variants (`combat::Active()`); every other APK keeps the game's own level name.
 *
 * Switch (ADR 0046 rule 4): `combat.levelname` in config.yaml, default true. The plugin's kill file `levelname.off` is no longer read: one found in
 * the plugin's data directory is a Warning (`combat_levelname_legacy_switch`) naming the key.
 *
 * Frequency (ADR 0038). The function runs once per level the game loads: the handler compares the level to one constant and reads the flags word
 * only for global_mp. Not measured on a headset.
 *
 * Origin. The reference build's G7 rule (its patch table `full_global_mp_patches`, tools/qcp_patches.py:389; docs/audits/2026-11-05-q15t-origin-citations.md,
 * rows for `levelname`), which rewrote the `tbnz` branch target into a code cave. This is our own implementation, reached through the vtable
 * words instead of a code write, imported with the combat plugin (#1591) from kotorvr's echoquest-combat, from that plugin's levelname.cpp.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_levelname_install","result":"installed|refused|off","reason":"ok|off_config|<GotStatus token>|<status token>","key":"combat.levelname",
 *             "slots":<installed words>,"failed_slot":"<name>|none","cr15_game":"<GotStatus>","dedicated_server":..,"lan_server":..,"net_server":..,
 *             "rolled_back":<n>,"rollback_failed":<n>,"consequence":".."}   Info when installed or off, Error when refused
 *   kept     {"event":"combat_levelname_kept","name":"0x3f9915d3001dc28e","n":<count>,"lowspec":1,"answer":"plain name"}   Info, the first 16 only, written
 *            after the plain function returned (callback_thunk.h rule 1); `KeptPlainCount()` keeps counting
 *   cap      {"event":"combat_levelname_log_cap","lines":"kept","cap":16,"msg":".."}   Warning, once: written by the 17th substitution (line_budget.h)
 *   stats    {"event":"combat_levelname_stats","kept_plain":<n>,"final":1,"consequence":".."}   Info, once at the end of the process, when installed
 *   legacy   {"event":"combat_levelname_legacy_switch","file":"levelname.off","replaced_by":"combat.levelname: false",..}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/levelname_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::levelname {

// The module's normal step. `on` is `combat.levelname`. installed is true when all four words hold the thunk. Fixed status tokens:
// "installed", "refused", "off", "already_ran", "module_not_loaded". A refused step leaves nothing installed and may be tried again.
ModuleResult Install(bool on) noexcept;

// The same on the image `lookup` finds. The plugin's old kill file is looked for through legacy::Present (legacy_switch.h), in the directory that
// the test seam (legacy::SetDirectoryForTest) names, or the plugin's own data directory.
ModuleResult InstallAt(sentinel::ImageLookup lookup, bool on) noexcept;

// True once Install put all four thunks in and until ResetForTest: the companion (boosterpool.h) installs only then.
bool Installed() noexcept;

// The table's end-of-process step (combat_install.h): writes the final combat_levelname_stats line when the module is installed, nothing otherwise.
void NoteEnd() noexcept;

// global_mp loads that were given the plain name since install.
std::uint64_t KeptPlainCount() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// The plain function the handler calls (production: base + the stub, set by the install) and what it is now.
void SetPlainForTest(std::uintptr_t plain) noexcept;
std::uintptr_t PlainForTest() noexcept;
// Takes the four thunks back out (a refusal of a word someone else changed is forgotten), disarms them and forgets the install.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::levelname

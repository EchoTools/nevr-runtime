/* The booster pool's actor keeps its plain name (#2181, the companion of levelname.h; docs/quest/combat/levelname.md): with the full-size global_mp loaded,
 * CR15NetBoosterCS::AllocateExclusive asks for the plain pooled actor, not the "_lowspec" one the cut level carried.
 *
 * What the game does. AllocateExclusive (store libr15 0x2110dd4, boosterpool_targets.h) asks `CncaGameSpace::AllocatePooledActor(st, name)` for the
 * booster pool's actor. When bit 31 of the flags word at ((this+0x80)->+8)+0x7ad8 is set (a Quest's "lowspec") the name is
 * `CSymbol64::Append(plain, "_lowspec", 1, 1)`, an actor only the cut global_mp has. With the full-size global_mp that levelname.h loads, only the
 * plain actor exists, the allocation finds nothing and the booster's BodyTypeExclusionFlags reads a null component: the match crashes 0.7 s in.
 *
 * What this does. AllocatePooledActor is reached through one JUMP_SLOT (boosterpool_targets.h), which all its twelve callers use. The handler,
 * installed through the hook manager (`InstallThunk`, ADR 0046 rule 2), computes the name the stock would have built, `Append(plain, "_lowspec", 1, 1)`,
 * and when the caller's name equals it, calls the function the word held with the plain name instead. Every other name from every other caller
 * reaches the function unchanged. The two names are computed once, at the first call that can read the plain name (it is a constant of the
 * game's data once the library is up), so a call costs one load and one compare.
 *
 * Only with levelname. The substitution is right only while the full-size level is the one loaded, so this module installs only when levelname.h
 * installed (`needs_levelname` otherwise): with the cut level the plain actor does not exist and the substitution would be the crash.
 *
 * Arena (same APK). The handler changes nothing for a name other than the one suffixed name. An arena match is not scoped away: with the full-size
 * level loaded (levelname.h, which cannot be scoped by level), an arena call for the suffixed name would find no actor, so the substitution is what
 * keeps it from the same crash. The cost is on every caller of AllocatePooledActor, arena's included; see docs/quest/combat/levelname.md ("What an
 * arena match gets").
 *
 * Which APKs. The q15t and combat variants (`combat::Active()`), and only where `combat.levelname` is on; with it off, or with levelname not
 * installed, the game keeps its allocations.
 *
 * Switch (ADR 0046 rule 4): `combat.levelname`, the same key as levelname.h (the two are one feature), default true.
 *
 * Frequency (ADR 0038). Twelve call sites (bullet and magazine spawns, bot and remote-user actors, marker actors, the booster's pool actor and
 * two functions named ExclusiveUpdate); the call rate per frame was not measured. The handler's added work is the thunk's call counter and one
 * atomic load and compare per call once the names are cached; before that, one process_vm_readv and one Append call, at most a few times.
 *
 * Origin. The reference build's `booster_pool.plain_name` word row (its patch table, `tbz` at 0x2110f40 stock 0x36f8012a to `b` 0x14000009;
 * docs/audits/2026-11-05-q15t-origin-citations.md, the `levelname` rows). The word row is not applied here: this is our own implementation, reached
 * through the import slot, moved from the q15t plugin's boosterpool.cpp (imported from kotorvr's echoquest-combat).
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_boosterpool_install","result":"installed|refused|off","reason":"ok|off_config|needs_levelname|<GotStatus token>|<status token>","key":"combat.levelname",
 *             "slot":"<GotStatus>","consequence":".."}   Info when installed or off, Error when refused
 *   kept     {"event":"combat_boosterpool_kept","name":"0x..","n":<count>,"answer":"plain name"}   Info, the first 16, written after the allocation returned
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/boosterpool_targets.h"
#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::boosterpool {

// The module's normal step. `on` is `combat.levelname`; `levelnameInstalled` is whether levelname.h installed. installed is true when the word holds
// the thunk. Fixed status tokens: "installed", "refused", "off", "needs_levelname", "already_ran", "module_not_loaded".
ModuleResult Install(bool on, bool levelnameInstalled) noexcept;
ModuleResult InstallAt(sentinel::ImageLookup lookup, bool on, bool levelnameInstalled) noexcept;

// Allocations whose suffixed name was replaced by the plain one since install.
std::uint64_t KeptCount() noexcept;
// Calls that passed through unchanged because the plain name could not be read.
std::uint64_t UnreadableCount() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// What the handler calls and reads (production: base + the stub and base + the plain name's address): the Append function and the address of the
// plain name. Forgets the names it cached.
using AppendFn = std::uint64_t (*)(std::uint64_t name, const char* suffix, std::uint32_t a, std::uint32_t b);
void SetTargetsForTest(AppendFn append, std::uintptr_t plainAddress) noexcept;
void TargetsForTest(AppendFn* append, std::uintptr_t* plainAddress) noexcept;
// Takes the thunk out, disarms it and forgets the install and every name it cached.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::boosterpool

/* The wall-fire check (#2182, docs/adr/0046; docs/quest/combat/wallfire.md): a gun's shot is refused when the segment from the player to the
 * muzzle crosses static level geometry, as on the PC. It depends on bvh.h (the static BVH has to answer CBVHTree::Occluded).
 *
 * One hook (wallfire_targets.h), a CallbackThunk on CncaGameSpace::QueryCollision's JUMP_SLOT: the game's function runs; only when it returns to
 * FireGunLocal's call site and found nothing, and a combat map is loaded, the check asks the engine's own static query,
 * CncaGameSpace::StaticRaycastOccluded (called directly: its JUMP_SLOT is read at install, never hooked), for the same segment. The stub reaches
 * CBVHTree::Occluded through CncaGame's virtual +0x278, which is bvh's Occluded hook. A segment that is blocked makes QueryCollision answer 1, and
 * FireGunLocal returns without firing. Every other caller of QueryCollision gets the game's answer.
 *
 * Switch (docs/adr/0046 C2): `combat.wallfire` (default true, as the reference build has it; it needs `combat.bvh` and installs nothing without it). The plugin's kill file `wallfire.off` is no longer read: one found is a Warning (`combat_wallfire_legacy_switch`) that names the key.
 *
 * Owner (ADR 0039). This file owns the QueryCollision slot. The q15t plugin no longer installs it.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only (the normal step of combat_install.h), after bvh in the registration table.
 *
 * Arena (same APK). The check acts only at FireGunLocal's call site and only while `bvh::InCombatOrLobby()` (a combat map or the combat lobby, a fresh look); the arena path of a QueryCollision call
 * is the game's function, a compare of the caller with the site and a return.
 *
 * Reads. The request is the game's caller's stack frame (0x2255660 add x1, sp, #0x30): a plain load. The game's QueryCollision never reads `this`
 * (0x19067a0 mov x0, sp), so nothing the game does on this path backs `gs+8`, the game or its vtable +0x278: those three words are read with
 * SafeRead (about 1.5 us a shot, at the site only, after the scope test). The code word at the site and the occluded slot are read once, at install.
 *
 * Frequency (ADR 0038). QueryCollision runs once per collision query of the game's (the rate in arena and combat is not measured on a headset);
 * the check runs once per shot the stock query let through, a few a second per gun. Measured per call on the host: docs/quest/combat/wallfire.md "Cost".
 *
 * Origin. The reference build's runtime/native/afkxr/afk_wallfire.cpp (:1-13 the rule, :34-36 the pins), reconstructed on the manager; the commit is in
 * docs/audits/2026-11-05-q15t-origin-citations.md. The module is Ducky's: every address here is in upstream's afk_wallfire.cpp. The game-chain check
 * (`GameIsPinned`) is ours.
 *
 * Lines (sentinel log):
 *   switch    one {"event":"combat_config_switch","module":"wallfire","key":"combat.wallfire","on":0|1,"source":"default|config"}, generated from the registration table
 *             by the configuration step (combat_install.h); the module writes none
 *   install   {"event":"combat_wallfire_install","result":"installed|off|refused","reason":"..","msg":".."}   Info for installed and off, Error for a refusal
 *   blocked   {"event":"combat_wallfire_blocked","blocked":<n>,"checked":<n>,"segment":"x y z -> x y z"}   the first 16, then at powers of two
 *   skipped   {"event":"combat_wallfire_skipped","reason":"game_chain",..}   Warning, once: CncaGame's virtual +0x278 is not the pinned function
 *   legacy    {"event":"combat_wallfire_legacy_switch","file":"wallfire.off","replaced_by":"combat.wallfire: false"}   Warning
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/wallfire_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::wallfire {

// The step on the loaded libr15. Fixed status tokens: "installed", "off_config", "already_ran", "bvh_not_installed", "module_not_loaded",
// "site_unreadable", "site_word", "occluded_unreadable", "occluded_slot", or the manager's status name of the hook that was refused.
ModuleResult Install(bool on) noexcept;
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept;

struct Counters {
  std::uint64_t atSite;        // QueryCollision calls from FireGunLocal's site whose stock answer was "nothing"
  std::uint64_t checked;       // ... of those, the ones whose segment and CncaGame were as pinned, so the static level was asked
  std::uint64_t blocked;       // ... and the static level blocked it
  std::uint64_t skippedNan;    // at-site calls with a non-finite segment
  std::uint64_t skippedGame;   // at-site calls whose game structure chain is null or whose CncaGame virtual is not the pinned function
};
Counters GetCounters() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
void SetPinsForTest(const Pins* pins) noexcept;
// The return address (an ELF vaddr) of the last QueryCollision call the hook saw while a combat map was loaded.
std::uintptr_t LastReturnForTest() noexcept;
// Forgets the hook, the thunk and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::wallfire

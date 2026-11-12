/* POWER BOOST, the client half (#2187, docs/adr/0046, docs/quest/combat/powerboost.md; rows R51..R56). An EchoCombat server puts POWER
 * BOOST's messages into the heal channel: a heal amount of 1,000,000 or more is never a heal, it is a marker ("I used it", "you are
 * linked", "unlinked"). Without this module those markers reach CR15NetDamageableCS::Heal as enormous heals.
 *
 *   stage 1 (read-only): wrappers on CR15NetDamageableHealNode::Enter, CR15NetDamageableCS::Heal (its JUMP_SLOT) and
 *     CR15NetHealBubbleCS::SetTargetInsideBubble record where marker amounts pass and what the heal bubble's link call is given. Every
 *     call goes on to the game's function untouched.
 *   stage 2 (swallow): a marker never reaches Heal; the node's queued heal (Heal's GLOB_DAT word) is caught, and a "used it" marker is
 *     sent to the server as DamageFromClient(cs, handle, -amount, 0, 0).
 *   stage 3 (cone): ResolveBody's JUMP_SLOT holds an assembly entry (powerboost_thunk.inc, callback_thunk.h rule 7) that acts on the one
 *     call UpdateActiveHealBubble makes per target (return address base + 0x215799c); a target outside the owner's +z cone gets a far
 *     stand-in body; link / unlink markers go out from the EC client's net tick (ec::OnNetTick). Without that tick stage 3 is refused,
 *     loudly, and stage 2 stays.
 *
 * Switch. `combat.powerboost_stage` in config.yaml, an integer 0..3, default 3: what kotorvr's echoquest-combat ships (its wave-2
 * recipe qa-1104-aj4-w2z22.json lists an empty powerboost.on, whose words are `swallow cone`, afk_powerboost.cpp:89). 0 installs nothing.
 *
 * Arena (same APK). Every install happens on the combat APKs, which also play Echo Arena. The module acts only while a combat map
 * or the combat lobby is loaded and no Echo Arena level is (InScope(): the game tick asks the combat level answer, combat_scope.h, once every
 * kScopeEveryFrames frames, one `LastLook()` record judged by `InCombatOrLobby`, and keeps the answer in an atomic; with no tick seen yet, the answer
 * is "not combat"). Each wrapper tests that atomic; every in-scope body (and the cone's continuation) then starts with `scope::LookIsFresh()`
 * (src/quest/combat/README.md rule 9), so while the tick is stopped a combat answer older than a second (kStaleAfterMs) passes every call on, though
 * the atomic still says combat. Anywhere else every
 * wrapper only calls the game's function, the entry passes every call on, and nothing is logged, held, swallowed or sent. The module is a reader
 * of the scope row (`combat.powerboost_stage` 1 to 3 is in combat_install.cpp
 * `ScopeNormal()`'s list), and the install refuses (`scope_not_live`) when the answer cannot change.
 *
 * Frequency (ADR 0038). ResolveBody has 771 callers and runs many times per frame: the entry's path for every call but the cone's is 10
 * instructions (bti, two adrp/add/ldar pairs, cmp, b.eq, br). Heal is called per damage-dealt message and per heal node; HealNode::Enter
 * per heal node a script runs; SetTargetInsideBubble once per bubble target per update of a heal bubble. Their thunks' arena path is the
 * thunk's counter, two acquire loads, one acquire load of the scope and the game's call. In scope each call adds one clock read
 * (`LookIsFresh`, CLOCK_MONOTONIC through the vDSO, no system call) before its work; docs/quest/combat/powerboost.md "Arena" gives the host cost.
 *
 * Owner (ADR 0039). This file owns the five words.
 *
 * Origin. The reference build's runtime/native/afkxr/afk_powerboost.cpp (1357409; docs/audits/2026-11-05-q15t-origin-citations.md rows
 * 244-254, 273), and the EchoCombat plugin/powerboost.cpp rules it ports: the cone filter :574-623, the link state :487-505, the
 * owner's 0.3 m :585, the +z axis :224, the diagnostic rate :610-620, the 30 s use window :500-504. runtime/patch/powerboost_rules.h names the rest.
 *
 * Lines (sentinel log), each a `combat_powerboost_*` event:
 *   hook       {"hook":"node|heal|queued_heal|link|resolve","slot_vaddr":"0x..","status":"<GotStatus>"}   Info, Error when refused
 *   install    {"result":"installed|partial|refused|off","stage":<n>,"asked_stage":<n>,"reason":"..","consequence":".."}
 *   marker     {"kind":<n>,"rez":..,"v":..,"dr_permille":..,"seconds":..,"where":"node|heal","index":..,"user":..}    the first 96
 *   swallow    {"kind":..,"rez":..,"v":..,"where":..,"index":..,"what":".."}
 *   use        {"v":"..","index":..,"why":".."}                 a use sent; `held_dropped` when a held use is not sent
 *   link       {"bubble":..,"target":..,"x":"..","y":"..","z":"..","arg":..}   one per (bubble, target) change, at most once a second
 *   cone       {"target":..,"dist_m":"..","max_m":"..","off_deg":"..","max_deg":..,"verdict":"LINK|no"}   at most twice a second
 *   link_marker {"direction":"link to|unlink from","index":..,"marker":".."} sent, or {"why":".."} not sent
 *   capped     {"kind":"..","cap":<n>}   once per kind, when its cap is reached
 *   scope      {"in_combat":0|1,"kind":"combat_or_lobby|echo_arena|other|no_look|stale","level":"0x.."}   each change of the class, the first 64
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/powerboost_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::powerboost {

struct PhTarget {  // CPhTarget, 24 bytes (returned through x8 by CPhysicsCS::PrimaryBody)
  std::uint64_t w[3];
};

struct Fns {
  // CR15NetDamageableCS::DamageFromClient(cs, handle, float amount (s0), hit body (w2), symbol (x3))
  void (*send)(void* cs, std::uint64_t handle, float amount, std::uint32_t hitBody, std::uint64_t symbol);
  const std::uint8_t* (*resolve)(const PhTarget* target);       // CPhTarget::ResolveBody (the real one)
  PhTarget (*primary)(const void* physics, std::uint32_t handle);  // CPhysicsCS::PrimaryBody(SCompactPoolHandle) const
};

enum Stage : int { kOff = 0, kRead = 1, kSwallow = 2, kCone = 3 };
inline constexpr int kDefaultStage = kCone;
// The scope is asked again once every this many game frames (about 2.25 times a second at 72 Hz: 32 frames is about 444 ms).
inline constexpr std::uint32_t kScopeEveryFrames = 32;

// The three engine functions by exported name (dlsym on the loaded libr15.so), each required to be at base + its pin. False when one is
// missing or elsewhere.
bool ResolveFns(std::uintptr_t base, const Pins& pins, Fns* out) noexcept;

// The step on the loaded libr15 with `stage` (combat.powerboost_stage). Fixed status tokens: "installed", "partial", "refused", "off",
// "already_ran", "scope_not_live", "module_not_loaded", "no_build_id", "build_id_mismatch", "no_thread_key", "tick_table_full", "hook_failed".
ModuleResult Install(int stage) noexcept;
// The same on the image `lookup` finds, with `pins`, and `fns` (null: ResolveFns).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, int stage, const Fns* fns) noexcept;

// The game tick (combat_tick.h): the CR15Game, for the scope and for the net tick's live-system check.
void GameTick(std::uintptr_t game) noexcept;
// The net tick (ec::OnNetTick): stage 3's link / unlink sends. One acquire load when nothing is pending.
void NetTick() noexcept;
// Whether the module may act now: a combat map or the combat lobby is loaded (the cached answer of the game tick).
bool InScope() noexcept;

struct Stats {
  std::uint32_t markers, swallowed, sent, held, links, coneCalls, coneRejects, pendingSent, pendingDropped, scopeChecks;
  int stage;       // the stage that is on (1..3), 0 when nothing is installed
  bool coneArmed;  // the entry is in ResolveBody's slot and the cone is live
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

void SetClockForTest(std::uint64_t (*nowMs)()) noexcept;
void SwallowForTest(bool on) noexcept;  // the flag the queued-heal wrapper reads
// What the entry's continuation runs for the one call it acts on.
const std::uint8_t* ConeAtForTest(const PhTarget* target, const std::uint8_t* frame, std::uintptr_t dcs) noexcept;
// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
void SetPinsForTest(const Pins* pins) noexcept;
// The ec net-tick registration the install calls (default ec::OnNetTick); nullptr restores it.
void SetNetTickRegistrarForTest(bool (*reg)(void (*)() noexcept) noexcept) noexcept;
// The two words the entry reads.
std::uintptr_t ConeRetForTest() noexcept;
std::uintptr_t NextForTest() noexcept;
// Forgets the hooks, the thunks, the tables, the scope and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::powerboost

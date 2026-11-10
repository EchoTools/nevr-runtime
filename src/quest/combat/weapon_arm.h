/* A profile whose `weaponarm` is not 1 gets the gun on the right hand, once per user id (#2185, docs/adr/0046; docs/quest/combat/adr/0003-weapon-arm-new-profile.md).
 * Runs on the game-thread tick (combat_tick.h), 1 Hz until done. The profile id is CR15NetGame::ClientProfile()'s "xplatformid"; a marker file
 * `t26arm.<id>` in the player's data directory means this profile was handled before and nothing is done. Otherwise, in a combat map, the local equipment
 * component system is found in the gamespace tables (gamespace.h), held for 3 passes, and (the reference build's rule, T-26)
 *   - GetWeaponArm == 1, or the profile's `weaponarm` == 1: the marker only (the gun is on the right already);
 *   - else (`weaponarm` 0, or no such key): SetWeaponArm(cs, handle, 1) once (direct when [gs+0x368] is set, else through the gamespace's deferred
 *     method queue), and the marker once GetWeaponArm reads 1 (within 10 s, else no marker: the next launch retries).
 * The community service sends `weaponarm` for every profile and an account that never chose reads 0, so "never chose" and "chose left" are the same value and
 * both are set. The cost: a left-handed player's gun is moved to the right once per missing marker (a reinstall, cleared app data, or a move between the store
 * and the combat APK; Android clears the app's external files directory on an uninstall, inferred, not measured), and the player switches it back.
 * SetWeaponArm is saved into the profile's `weaponarm` key by the game's SavePlayerEquipment, so a set is a change to the saved profile.
 *
 * Player state (ADR 0046 rule 4). The marker directory is `/sdcard/Android/data/<package>/files/q15t`, where the plugin's markers already are; it is
 * state, not a switch, and renaming it would make every profile look unhandled. A marker the plugin wrote stops this module from touching that
 * profile (weapon_arm_test). The marker file is the module's only write.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only; with `combat::Active()` false the install step never runs.
 *
 * Switch. `combat.weapon_arm` in config.yaml, on unless the file says false (the reference build's T-26 is on in its shipped build,
 * afk_gunconnect.cpp:58-64,587; the q15t plugin had it on). The headset runs that loaded the plugin's copy wrote the marker for a profile already on the
 * right hand and found the marker on later runs, and never a `set`, so its headset row requires `combat_weapon_arm_set`. The plugin's kill file
 * `t26arm.off` is no longer read; one found is a Warning (`combat_weapon_arm_legacy_switch`).
 *
 * Arena (same APK). The module reads the loaded levels and goes on only while a combat map is loaded and no Echo Arena level is (combat_level.h `Classify`,
 * without the lobby): in an arena match, in every lobby (the combat lobby too: the lobby script is shared with the arena lobby, and it is not established that an
 * arena player never has mpl_lobby_b_combat loaded) and in a transition that still holds an arena level it neither looks for the equipment system nor sets the
 * hand nor writes the marker, and writes no line. A profile that has a marker never scans the levels: the marker check comes first and ends the module. A
 * profile with no marker that only ever plays arena is looked at once a second and never touched. KNOWN-WRONG #2168: the scope asks `combat_level` directly; it
 * moves to combat_scope (F19, #2246) when that lands.
 *
 * Frequency (ADR 0038). With the switch off nothing is registered: no frame runs any of this. With it on, the tick callback runs once per frame:
 * once the module is done, one atomic load and a branch; before that, one clock read and one compare, and a pass at 1 Hz (the netgame, the profile
 * id, the marker check and, for a profile with no marker, the level scan; in a combat level also the equipment search and three engine calls).
 * Not measured on a headset; weapon_arm_test prints the host cost of the arena pass.
 *
 * Origin. The reference build's T-26 in kotorvr's echoquest-combat repository (commit c5a67e8): runtime/native/afkxr/afk_gunconnect.cpp:58-64 (the
 * header), :496-598 (the pass), which the q15t plugin reconstructed (`weapon_arm.cpp`); this file moves that reconstruction into the runtime.
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_weapon_arm_install","result":"installed|off|refused","reason":"..","msg":".."}
 *   waiting   {"event":"combat_weapon_arm_waiting","why":".."}   one per distinct reason while looking
 *   set       {"event":"combat_weapon_arm_set","route":"direct|deferred","id_tail":"..","arm":<n>,"profile_weaponarm":<n>}
 *   done      {"event":"combat_weapon_arm_done","outcome":"..","marker":"written|present|write_failed","id_tail":"..","arm":<n>,"profile_weaponarm":<n>}
 *   gave up   {"event":"combat_weapon_arm_gave_up","id_tail":"..","arm":<n>}   Warning: the set did not take; no marker, the next launch retries
 *   marker    {"event":"combat_weapon_arm_marker_failed","id_tail":".."}   Error: without the marker the set repeats on every launch
 *   legacy    {"event":"combat_weapon_arm_legacy_switch","file":"t26arm.off","replaced_by":"combat.weapon_arm","msg":".."}   Warning
 * A profile id is never logged whole, only its last four characters.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::weapon_arm {

struct Fns {
  void* (*netGame)(void* game);                                                            // CR15Game::NetGameMT
  void* (*clientProfile)(void* net);                                                       // CR15NetGame::ClientProfile
  const char* (*tString)(void* json, const char* key, const char* dflt, unsigned flags);  // CJson::TString
  long long (*jsonInt)(void* json, const char* key, long long dflt, unsigned flags);      // CJson::Int
  bool (*isValid)(void* cs, std::uint64_t handle);                                        // CR15NetPlayerEquipmentCS::IsComponentValid
  std::uint32_t (*getArm)(void* cs, std::uint64_t handle);                                // ::GetWeaponArm
  void (*setArm)(void* cs, std::uint64_t handle, std::uint64_t arm);                      // ::SetWeaponArm
  void (*invoke)(void* functor, char* buf);                                               // CDeferredMethodQueue::SFunctor2<...>::InvokeMethod
  void* (*queue)(void* queueObj, void (*invoke)(void*, char*));                           // CDeferredMethodQueue::QueueFunctor<...>
  int named;                                                                              // how many of the nine were confirmed by name (log only)
};

using SymbolLookup = void* (*)(const char* symbol);

// The nine functions on a libr15 loaded at `base`. A name that resolves anywhere but its pinned address is a refusal (false, `why` is the name); a
// name that does not resolve is accepted at the pinned address.
bool Resolve(std::uintptr_t base, SymbolLookup lookup, Fns* out, const char** why) noexcept;

// The module's normal step. Off: one Info line, nothing registered. On: finds the loaded libr15, checks its build id, resolves the nine functions and
// registers the tick callback.
ModuleResult Install(bool on) noexcept;
ModuleResult InstallAt(sentinel::ImageLookup lookup, SymbolLookup symbols, bool on) noexcept;
// The part after the resolution (a test calls it with fake functions).
ModuleResult InstallResolved(std::uintptr_t base, const Fns& fns) noexcept;

// The tick callback (combat_tick.h).
void Tick(std::uintptr_t game) noexcept;

// 0 looking, 1 set sent (verifying), 2 done (or off or refused).
int Phase() noexcept;

struct Stats {
  std::uint32_t passes;     // passes that ran (past the clock)
  std::uint32_t scopedOut;  // passes that found no combat level and did nothing
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

void SetClockForTest(double (*clock)()) noexcept;
void SetLegacyDirForTest(const char* dir) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::weapon_arm

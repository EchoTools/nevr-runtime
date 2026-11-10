# ADR 0003: a profile whose `weaponarm` is not 1 gets the gun on the right hand, once per marker

## Decision

A profile whose `weaponarm` is not 1 gets its gun on the right hand, once per user id and missing marker, by the reference build's rule (T-26, `afk_gunconnect.cpp:58-64,615-662`). It is combat behaviour of the
runtime (`src/quest/combat/weapon_arm.h`, docs/quest/combat/weapon_arm.md), on by default (`combat.weapon_arm`), and not an A/B arm. It runs on the combat game tick and writes no code word.

It writes player state: the game's `SavePlayerEquipment` stores the arm into the profile's `weaponarm` key, so the set persists. The community service sends `weaponarm` for every profile (nakama's client
profile has the key without `omitempty`) and an account that never chose reads 0 (inferred: the loadout's zero value; no captured profile of a new account yet), so "never chose" and "chose left" are the same value
and cannot be told apart. The rule therefore treats `weaponarm` 0 and an absent key alike. The cost: a left-handed player's gun is moved to the right once per missing marker (a reinstall, cleared app data, or a switch
between the store and the combat build), and the player switches it back.

## Behaviour

1 Hz on the game thread until done. The profile id is `CR15NetGame::ClientProfile()`'s `xplatformid`. A marker file `t26arm.<id>` in `/sdcard/Android/data/<package>/files/q15t/` means the
profile was handled in an earlier launch: nothing is done. Otherwise, in a combat map, the local `R14NetPlayerEquipment` component system is found in the two gamespace tables, held for 3
passes, and then:
- `GetWeaponArm == 1`: the marker only;
- the profile's `weaponarm` is 1: the marker only;
- else (`weaponarm` 0 or no such key) `SetWeaponArm(cs, handle, 1)` once: direct when `[gs+0x368]` is set, else through the gamespace's deferred method queue (`gs+0x170`), with the functor layout of the game's own script node;
- the marker is written once `GetWeaponArm` reads 1; after 10 s without that, a Warning and no marker.

## Why the marker directory is not renamed

The markers are the player's state, not a switch (docs/adr/0046 rule 4). Every existing profile has its marker under `files/q15t`; a module that looked elsewhere would find none. With the profile read
first would repeat the 3-pass hold, the set and the marker write for every profile, so the directory stays.

## A missing marker

The marker directory is under the app's external files directory, which Android clears on uninstall (inferred), and a move between the store and the combat APK is an uninstall (another signing key). Every
profile then has no marker, and each one whose `weaponarm` is not 1 is set again: a left-handed player's gun moves to the right once per such event.

## Why it waits for a combat map

Arena and combat run on the same APK. A profile's gun hand is only meaningful in combat; setting it, or writing the marker, while an arena match is loaded would be a change the arena player did not ask
for. The combat lobby is not in scope either: its script is shared with the arena lobby, and it is not established that an arena player never has `mpl_lobby_b_combat` loaded. The module does nothing until a
combat map is loaded and no Echo Arena level is.

## Evidence

`src/quest/tests/weapon_arm_test.cpp`: the hold of 3 passes; the direct and the deferred route with the functor's bytes; the already-right path; a profile with `weaponarm` 0 (set), 1 (marker only, also after the marker is gone) and no key (set), on the direct and the deferred route; a marker from an earlier launch
(whatever its text, never rewritten); the give-up after 10 s; an invalid component; a wrong vptr, key or gamespace link; no netgame; a data directory that cannot be written; one pass per launch;
the switch; the kill file warning; the scope; the install on an image. `src/quest/combat/mutants.d/weapon_arm.sh` kills its mutants.

## Prediction (not run on a headset)

On a profile with no marker whose `weaponarm` is 0 (what a new account on the community service shows, inferred) or absent, and whose gun is on the left: `combat_weapon_arm_install`, then within about 10 s of the equipment component existing in a combat map
`combat_weapon_arm_set` and `combat_weapon_arm_done` (`weaponarm now 1`), and a file `t26arm.<id>`. Falsifier: `set` without `done` and a `gave_up` Warning; or no `set` after the component
exists. The second launch logs `marker: present` and changes nothing. A profile already on the right gets the marker and no `set` (`outcome: weaponarm right already, nothing changed`, or `profile weaponarm is 1, nothing changed` when only the profile holds the right hand). No crash and no change to frame timing.

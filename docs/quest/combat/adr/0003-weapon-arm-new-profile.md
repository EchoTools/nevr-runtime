# ADR 0003: a new profile starts with the gun on the right hand

## Decision

A profile the module has not handled before gets its gun on the right hand, once per user id, as the reference build does (T-26, `afk_gunconnect.cpp:58-64`). It is combat behaviour of the
runtime (`src/quest/combat/weapon_arm.h`, docs/quest/combat/weapon_arm.md), on by default (`combat.weapon_arm`), and not an A/B arm. It runs on the combat game tick and writes no code word.

## Behaviour

1 Hz on the game thread until done. The profile id is `CR15NetGame::ClientProfile()`'s `xplatformid`. A marker file `t26arm.<id>` in `/sdcard/Android/data/<package>/files/q15t/` means the
profile was handled in an earlier launch: nothing is done. Otherwise, in a combat level, the local `R14NetPlayerEquipment` component system is found in the two gamespace tables, held for 3
passes, and then:
- `GetWeaponArm == 1` or the profile's `weaponarm == 1`: the marker only (the player's own right-hand choice stands);
- else `SetWeaponArm(cs, handle, 1)` once: direct when `[gs+0x368]` is set, else through the gamespace's deferred method queue (`gs+0x170`), with the functor layout of the station's hand swap;
- the marker is written once `GetWeaponArm` reads 1; after 10 s without that, a Warning and no marker.

## Why the marker directory is not renamed

The markers are the player's state, not a switch (docs/adr/0046 rule 4). Every existing profile has its marker under `files/q15t`; a module that looked elsewhere would find none and set the
hand of every profile again, including one whose player moved the gun to the left after the first launch.

## Why it waits for a combat level

Arena and combat run on the same APK. A profile's gun hand is only meaningful in combat; setting it, or writing the marker, while an arena match is loaded would be a change the arena player did
not ask for. The module therefore does nothing until a combat map or the combat lobby is loaded and no Echo Arena level is.

## Evidence

`src/quest/tests/weapon_arm_test.cpp`: the hold of 3 passes; the direct and the deferred route with the functor's bytes; the already-right and profile-right paths; a marker from an earlier launch
(whatever its text, never rewritten); the give-up after 10 s; an invalid component; a wrong vptr, key or gamespace link; no netgame; a data directory that cannot be written; one pass per launch;
the switch; the kill file warning; the scope; the install on an image. `src/quest/combat/mutants.d/weapon_arm.sh` kills its mutants.

## Prediction (not run on a headset)

On a profile with no marker whose gun is on the left: `combat_weapon_arm_install`, then within about 10 s of the equipment component existing in a combat level `combat_weapon_arm_set` and
`combat_weapon_arm_done` (`weaponarm now 1`), and a file `t26arm.<id>`. Falsifier: `set` without `done` and a `gave_up` Warning; or no `set` after the component exists. The second launch logs
`marker: present` and changes nothing. A profile already on the right gets the marker and no `set`. No crash and no change to frame timing.

# ADR 0003: a profile that has never saved a gun hand starts with the gun on the right hand

## Decision

A profile that has never saved a gun hand gets its gun on the right hand, once per user id, as the reference build does for a new profile (T-26, `afk_gunconnect.cpp:58-64`). It is combat behaviour of the
runtime (`src/quest/combat/weapon_arm.h`, docs/quest/combat/weapon_arm.md), on by default (`combat.weapon_arm`), and not an A/B arm. It runs on the combat game tick and writes no code word.

It writes player state: the game's `SavePlayerEquipment` stores the arm into the profile's `weaponarm` key, so the set persists. It therefore acts only on a profile whose `weaponarm` key is absent. The reference
build acts on any profile without a marker, which would set the right hand on a left-handed player's profile after a reinstall; this module reads the profile first.

## Behaviour

1 Hz on the game thread until done. The profile id is `CR15NetGame::ClientProfile()`'s `xplatformid`. A marker file `t26arm.<id>` in `/sdcard/Android/data/<package>/files/q15t/` means the
profile was handled in an earlier launch: nothing is done. Otherwise, in a combat map, the local `R14NetPlayerEquipment` component system is found in the two gamespace tables, held for 3
passes, and then:
- `GetWeaponArm == 1`: the marker only;
- the profile has a saved `weaponarm` (0 or 1): the marker only, the player's choice stands;
- else `SetWeaponArm(cs, handle, 1)` once: direct when `[gs+0x368]` is set, else through the gamespace's deferred method queue (`gs+0x170`), with the functor layout of the game's own script node;
- the marker is written once `GetWeaponArm` reads 1; after 10 s without that, a Warning and no marker.

## Why the marker directory is not renamed

The markers are the player's state, not a switch (docs/adr/0046 rule 4). Every existing profile has its marker under `files/q15t`; a module that looked elsewhere would find none. With the profile read
first that would no longer reset a left hand, but it would repeat the 3-pass hold and the marker write for every profile, so the directory stays.

## A missing marker

The marker directory is under the app's external files directory, which Android clears on uninstall (inferred), and a move between the store and the combat APK is an uninstall (another signing key). Every
profile then has no marker. Because the module reads the profile's own `weaponarm`, a missing marker sets nothing for a profile that has saved a hand.

## Why it waits for a combat map

Arena and combat run on the same APK. A profile's gun hand is only meaningful in combat; setting it, or writing the marker, while an arena match is loaded would be a change the arena player did not ask
for. The combat lobby is not in scope either: its script is shared with the arena lobby, and it is not established that an arena player never has `mpl_lobby_b_combat` loaded. The module does nothing until a
combat map is loaded and no Echo Arena level is.

## Evidence

`src/quest/tests/weapon_arm_test.cpp`: the hold of 3 passes; the direct and the deferred route with the functor's bytes; the already-right path, a saved hand of 0 and of 1 (with and without a marker) and the never-saved control; a marker from an earlier launch
(whatever its text, never rewritten); the give-up after 10 s; an invalid component; a wrong vptr, key or gamespace link; no netgame; a data directory that cannot be written; one pass per launch;
the switch; the kill file warning; the scope; the install on an image. `src/quest/combat/mutants.d/weapon_arm.sh` kills its mutants.

## Prediction (not run on a headset; open until the rule is settled)

The rule for which profile is set is an open choice of the owner (see the PR): the rule above (a profile is set only when its `weaponarm` key is absent), or the reference build's (set when
`weaponarm` reads 0 and the component reads left). The community service sends `weaponarm` for every profile (nakama's client profile has the key without `omitempty`), so under the rule above
this module sets nothing there. The prediction below holds for the reference build's rule and for a source of profiles that omits the key.

On a profile with no marker whose gun is on the left and that the rule selects: `combat_weapon_arm_install`, then within about 10 s of the equipment component existing in a combat map
`combat_weapon_arm_set` and `combat_weapon_arm_done` (`weaponarm now 1`), and a file `t26arm.<id>`. Falsifier: `set` without `done` and a `gave_up` Warning; or no `set` after the component
exists. The second launch logs `marker: present` and changes nothing. A profile already on the right gets the marker and no `set`; one the rule leaves alone gets the marker and no `set`
(`outcome: profile has a saved gun hand, nothing changed`). No crash and no change to frame timing.

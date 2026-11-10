# Module `weapon_arm` (a profile whose `weaponarm` is not 1 gets the gun in the right hand, once per marker)

Files: `src/quest/combat/weapon_arm.{h,cpp}`, `weapon_arm_targets.h`, `gamespace.{h,cpp}`, `netgame_targets.h`, `src/quest/tests/weapon_arm_test.cpp`,
`src/quest/combat/mutants.d/weapon_arm.sh`. A normal combat step: the `q15t` and `combat` APKs only. Switch in `config.yaml`: `combat.weapon_arm`, on unless the file says false. The decision
record is `docs/quest/combat/adr/0003-weapon-arm-new-profile.md`.

## Origin

kotorvr's echoquest-combat repository (commit c5a67e8), `runtime/native/afkxr/afk_gunconnect.cpp`: the T-26 header :58-64 and the pass :496-598 (the equipment system key 0xca14940c84b03f6e
and vptr `0x3698200` are at :497-498). The q15t plugin reconstructed it (`weapon_arm.cpp`); this module is that reconstruction moved into the runtime, so the commits that move it credit kotorvr.

## Behaviour

A pass runs at 1 Hz on the game tick until the module is done. The profile id is `CR15NetGame::ClientProfile()`'s `xplatformid`. A marker file `t26arm.<id>` (the id's characters other than
letters, digits, `-` and `_` become `_`) in `/sdcard/Android/data/<package>/files/q15t` means the profile was handled before: nothing is done and the levels are never scanned. Otherwise, in a
combat map (below), the local equipment component system is found in the gamespace tables and held for 3 passes. Then:

- `GetWeaponArm == 1`: the gun is on the right already; the marker is written and nothing is set.
- the profile's `weaponarm` is 1 (`CJson::Int` answers -1 for an absent key): the marker is written and nothing is set.
- otherwise (`weaponarm` 0, or no such key): `SetWeaponArm(cs, handle, 1)` is called once (directly when `gs+0x368` is set, else through the gamespace's deferred method queue at `gs+0x170`, with the functor layout of the
  game's own SetWeaponArm script node), and the marker is written once `GetWeaponArm` reads 1. After 10 s without that: one Warning and no marker; the next launch retries.

**What the set changes.** `SetWeaponArm` stores the arm into the equipment component, and the game's `SavePlayerEquipment` writes that field into the profile under the key `weaponarm`. So a set is a change to the saved
profile, persistent from then on. Whether the profile's server copy changes is not established (the profile upload was not traced).

**A missing marker.** The marker directory is under the app's external files directory, which Android clears when the app is uninstalled (inferred, not measured), and the
combat APK is signed with another key than the store APK, so a move between them is an uninstall. After it every profile has no marker again, and the module acts on it as on a first launch. A marker that cannot be written is an Error (without it the set repeats on the next launch). A data directory that cannot be written is a
waiting line and no set.

## Defaults

The gun goes to the right hand because the reference build starts a new profile with the gun on the right, and the community service cannot say whether a player chose the left: it sends `weaponarm` for every profile (nakama's client profile
has the key without `omitempty`) and an account that never chose reads 0 (inferred; no captured profile yet), the same value as a player who chose left. So the module treats 0 and an absent key alike.
The cost is stated plainly: **a left-handed player's gun is moved to the right hand once for every missing marker**, that is after a reinstall, after clearing the app's data, and after a switch between the store build and
the combat build. They have to switch it back to the left in the menu. A profile whose `weaponarm` is already 1 is not touched. `combat.weapon_arm: false` turns the module off.

## Player state

The marker directory stays `files/q15t` (ADR 0046 rule 4): the markers are state, not a switch, and moving them would make every profile look unhandled. `weapon_arm_test` shows a marker the
plugin wrote, whatever its text, stops the module, is never rewritten, and that the module names its own marker as the plugin did. The plugin's kill file `t26arm.off` is no longer read; one found
is a Warning.

## Arena

The module goes on only while a combat map is loaded and no Echo Arena level is (`combat_level.h`, `Classify` without the lobby), and only for a profile with no marker. The combat lobby is not in scope: its script is shared with the arena
lobby (`headset_gates.md`), and it is not established that an arena player never has `mpl_lobby_b_combat` loaded. In an arena match, in every lobby and in a transition that still holds an arena level the module does not look for the equipment
system, does not set the hand, does not write the marker and writes no line. So it cannot change what an arena player sees or the game computes, and it never touches the profile of a player who only plays arena. Where it differs from the
plugin: the plugin set the hand wherever it first found the equipment system and for any profile without a marker; the module waits for the first combat map. A profile that only ever plays arena is looked at
once a second (the profile and the level scan) and is never touched.

## Cost (ADR 0038)

With the switch off nothing is registered. With it on, a done module costs one atomic load and a branch per frame (about 10 ns on the x86 host that built this change). Before it is done: a clock
read and a compare per frame, and a pass at 1 Hz. In an arena level a pass is the profile read and the level scan, about 3 us (x86 host). `weapon_arm_test` prints the numbers. Not measured on a headset.

## Lines

`combat_weapon_arm_install` (installed, off, or refused with its reason at Error), `_waiting`, `_set` (route `direct` or `deferred`), `_done` (outcome, marker `written`, `present` or `write_failed`),
`_gave_up` (Warning), `_marker_failed` (Error), `_pinned_word` (Warning), `_legacy_switch` (Warning). A profile id is logged by its last four characters only. The switch is also one
`combat_config_switch` record.

## Pins

`weapon_arm_targets.h` holds nine functions (the first, `NetGameMT`, is in `netgame_targets.h`) and the equipment system's vtable; the install checks the build id and the net game words and
refuses a name exported at another address than its pin (a name that does not resolve is taken at the pinned address). `got_pinned_test` checks every symbol against the store libr15.
The equipment key (the record of `R14NetPlayerEquipment`, 8 bytes at 0x2bb0318, returned by `CR15NetPlayerEquipmentCS::DefaultName`), `cs+0x130` (read by `SavePlayerEquipment`), `cs+0xc8` (read by `IsComponentValid`), `gs+0x368`, `gs+0x170` and the functor layout (the game's own SetWeaponArm script node, 0x23df0f4 to 0x23df164) are pinned by instruction words and checked at install and by `got_pinned_test`.

## Headset row

On a profile with no marker whose `weaponarm` is 0 (what a new account on the community service shows, inferred) or absent, and whose gun is on the left, on a combat APK: `combat_weapon_arm_install` with `result: installed`; within about 10 s of the equipment component existing in a combat level,
one `combat_weapon_arm_set` (`route` direct or deferred), then `combat_weapon_arm_done` with `outcome: weaponarm now 1`, and a file `t26arm.<id>` appears; in the match the gun is in the right hand.
Second launch: `combat_weapon_arm_done` with `marker: present` (`outcome: marker present: nothing changed`), nothing changed, also after the player set the gun left in the menu. A profile with a marker (the plugin's) gets `marker: present` and no
`set`; a profile whose `weaponarm` is 1 gets `done` with `outcome: profile weaponarm is 1, nothing changed` and no `set`. Falsifiers: `set` without `done` and a `gave_up` Warning (the call did not take, most likely the deferred queue layout); no `set` after the component exists (`waiting` says which gate); any
`set` or marker from an arena match. This row has not been run: the headset logs that held the plugin's copy show `done` with `weaponarm right already, nothing changed` and later `marker present`, never a `set`.

# Module `weapon_arm` (a new profile starts with the gun in the right hand)

Files: `src/quest/combat/weapon_arm.{h,cpp}`, `weapon_arm_targets.h`, `gamespace.{h,cpp}`, `netgame_targets.h`, `src/quest/tests/weapon_arm_test.cpp`,
`src/quest/combat/mutants.d/weapon_arm.sh`. A normal combat step: the `q15t` and `combat` APKs only. Switch in `config.yaml`: `combat.weapon_arm`, on unless the file says false. The decision
record is `docs/quest/combat/adr/0003-weapon-arm-new-profile.md`.

## Origin

kotorvr's echoquest-combat repository (commit c5a67e8), `runtime/native/afkxr/afk_gunconnect.cpp`: the T-26 header :58-64 and the pass :496-598 (the equipment system key 0xca14940c84b03f6e
and vptr `0x3698200` are at :497-498). The q15t plugin reconstructed it (`weapon_arm.cpp`); this module is that reconstruction moved into the runtime, so the commits that move it credit kotorvr.

## Behaviour

A pass runs at 1 Hz on the game tick until the module is done. The profile id is `CR15NetGame::ClientProfile()`'s `xplatformid`. A marker file `t26arm.<id>` (the id's characters other than
letters, digits, `-` and `_` become `_`) in `/sdcard/Android/data/<package>/files/q15t` means the profile was handled before: nothing is done and the levels are never scanned. Otherwise, in a
combat level (below), the local equipment component system is found in the gamespace tables, held for 3 passes, and then: `GetWeaponArm == 1` or the profile's `weaponarm == 1` writes the
marker only (the player's own right-hand choice stands); else `SetWeaponArm(cs, handle, 1)` is called once (directly when `gs+0x368` is set, else through the gamespace's deferred method queue
at `gs+0x170`, with the functor layout of the station's hand swap), and the marker is written once `GetWeaponArm` reads 1. After 10 s without that, one Warning and no marker: the next launch
retries. A marker that cannot be written is an Error (without it the set repeats on every launch). A data directory that cannot be written is a waiting line and no set.

## Player state

The marker directory stays `files/q15t` (ADR 0046 rule 4): the markers are state, not a switch, and moving them would arm every existing profile again. `weapon_arm_test` shows a marker the
plugin wrote, whatever its text, stops the module, is never rewritten, and that the module names its own marker as the plugin did. The plugin's kill file `t26arm.off` is no longer read; one found
is a Warning.

## Arena

The module goes on only while a combat map or the combat lobby is loaded and no Echo Arena level is (`combat_level.h`, `Classify` with the lobby), and only for a profile with no marker. In an arena
match, in the lobbies arena players use and in a transition that still holds an arena level it does not look for the equipment system, does not set the hand, does not write the marker and writes no
line. So it cannot change what an arena player sees or the game computes. Where it differs from the plugin: the plugin set the hand wherever it first found the equipment system; the module waits for
the first combat level. A profile that only ever plays arena is looked at once a second (the profile and the level scan) and is never touched.

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
`[UNVERIFIED]`: the equipment key, `cs+0x130`, `cs+0xc8`, `gs+0x368`, `gs+0x170` and the functor layout are the reference build's.

## Headset row

On a profile with no marker whose gun is on the left, on a combat APK: `combat_weapon_arm_install` with `result: installed`; within about 10 s of the equipment component existing in a combat level,
one `combat_weapon_arm_set` (`route` direct or deferred), then `combat_weapon_arm_done` with `outcome: weaponarm now 1`, and a file `t26arm.<id>` appears; in the match the gun is in the right hand.
Second launch: `combat_weapon_arm_done` with `marker: present`, nothing changed, also after the player set the gun left in the menu. A profile with a marker (the plugin's) gets `marker: present` and no
`set`. Falsifiers: `set` without `done` and a `gave_up` Warning (the call did not take, most likely the deferred queue layout); no `set` after the component exists (`waiting` says which gate); any
`set` or marker from an arena match. This row has not been run: the headset logs that held the plugin's copy show `done` with `weaponarm right already, nothing changed` and later `marker present`, never a `set`.

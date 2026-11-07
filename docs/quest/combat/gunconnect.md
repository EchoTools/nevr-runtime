# Module `gunconnect` (the local player's guns get `Connect(local user)`)

Files: `src/quest/combat/gunconnect.{h,cpp}`, `gunconnect_targets.h`, `gamespace.{h,cpp}`, `netgame_targets.h`, `pinned_words.h`, `src/quest/tests/gunconnect_test.cpp`,
`src/quest/combat/mutants.d/gunconnect.sh`. A normal combat step (`src/quest/combat/combat_install.h`): it runs in the `q15t` and `combat` APKs and in no other. Switch in
`config.yaml` (docs/adr/0046 rule 4): `combat.gunconnect`, on unless the file says false.

On Quest offline practice the local player's guns are never connected, so they do not fire. `CR15NetGunCS::Connect(cs, local user id)` connects every gun whose owner slot is that
user's; online the lobby-member event does it within a frame, offline nothing does. The module calls it from the game tick (`combat_tick.h`).

## Origin

kotorvr's echoquest-combat repository (commit c5a67e8), `runtime/native/afkxr/afk_gunconnect.cpp`: the header :1-24, the constants :91-93, `FindCs` :186-218, `ReadLocal` :222-240,
`HasCombatTag` :242-254, `CountGuns` :256-296, `FixPass` :312-358. The addresses are in that file at the same numbers as ours (`kFnConnect = 0x2153438, kFnHasTag = 0x1723760,
kFnUserActor = 0x1251654, kTagCombat = 0x37af288`, `kGunCsVptr = 0x3696050`), which are ELF virtual addresses; the disassembly's are 0x100000 more. The q15t plugin reconstructed
it (`gunconnect.cpp`); this module is that reconstruction moved into the runtime, so the commits that move it credit kotorvr. Not ported: the diagnostic and the tick-callback
registry.

## Behaviour

A pass is taken when the clock says it is due: every second, every quarter second while any gun system has unconnected local guns or cannot be read. A pass first asks the loaded
levels (below). In a combat level it finds the gun component systems in both gamespace tables (key, vptr `base + 0x3696050`, `cs+0x80 == gs`; at most 8), forgets the state of systems
that are gone, reads the local member (`ReadLocal`: the net game, `net+0xde` and `net+0xdc` non-zero, slot = u16 `member+0x178 & 0x1f`, generation, member index not `0xffff` and below the
member count; user id = `slot | gen << 16`; actor GUID = `UserActor(net, member index)`), and for each system counts the guns (owner slot == local slot, flag `props+0x9c` non-zero, state
`inst+0xd8 != 0` is unconnected). Per system: (a) some local gun unconnected, (b) the ring `cs+0x1c0` empty, (c) `HasTag(game, &guid, *kActorTagCombat)`. All three for 2 s and the
retry gap over: `Connect(cs, user id)`, retry after 1 s, up to 10 tries, then one Warning and no more for that system. (a) false after a try: one `combat_gunconnect_connected` line and the
tries start again. Any of (a) to (c) false restarts the 2 s hold; an unreadable local member restarts every hold.

## Arena

Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in. The pass acts only while a combat map or the combat lobby is loaded and no Echo Arena
level is (`combat_level.h`, `Classify` with the lobby): in an arena match, in the lobbies arena players use, in a transition that still holds an arena level and when the game cannot be read,
it finds nothing to do, calls nothing of the game's and writes no line. `gunconnect_test` shows it for eight such levels and a transition, and the module acting on the four combat maps and
the combat lobby. Online it does nothing in any level because (a) is false. The plugin's copy was not scoped by level; the combat tag (c) was its only scope.

## Cost (ADR 0038)

With the switch off nothing is registered, so no frame runs any of it. With it on, the tick callback runs once per rendered frame and does one clock read and one compare (about 7 ns on the
x86 host that built this change). A pass runs at 1 Hz: in an arena level that is the level scan alone, about 3 us (x86 host), so about 3 us a second; in a combat level it adds the gun
system search and the member read, a few more process_vm_readv reads, 4 Hz while a gun is unconnected. `gunconnect_test` prints both numbers. Not measured on a headset.

## Lines

`combat_gunconnect_install` (installed, off by key, or refused with its reason and the Error level), `_gun_cs`, `_waiting`, `_connect`, `_connected`, `_gave_up` (Warning),
`_pinned_word` (Warning), `_legacy_switch` (Warning: the plugin's `gunconnect.off` is no longer read, `combat.gunconnect` decides). The switch is also one `combat_config_switch` record.

## Pins

`gunconnect_targets.h` holds the three functions, the symbol, the vtable, 23 instruction words and the key's 8-byte record that load, store or are each offset and constant the module reads, tied to them by `static_assert`
(`pinned_words.h`); `netgame_targets.h` holds the net game step it shares with aibots and weapon_arm, and `gamespace_targets.h` the component-system lookup it shares with weapon_arm. The install checks the build id and every word and refuses on a difference, and a
name the game exports at another address than its pin is a refusal. `got_pinned_test` checks the symbols and the words against the store libr15 (`just test-quest-hooks-pinned`).
The gun system's key is the record of the name `R14NetGun` (the 8 bytes at 0x3184438, which `CR15NetGunCS::DefaultName` returns), the gamespace list offsets are the ones the game's own component-system lookup reads
(`gamespace_targets.h`), the ring at `cs+0x1c0` and the state at `inst+0xd8` are what `ExclusiveUpdateConnect` reads; each is pinned by a word or an 8-byte record and checked at install. Still `[UNVERIFIED]`: `net+0x138` (the member index of a
slot; no direct load found) and that `inst+0xd8 == 0` is what `Connect` stores for a connected gun. `net+0xde` is the local-user count (`PartyCount`), an inference that member 0 is the local user.

## Headset row

Log only, no player-visible change beyond the guns firing, on a combat APK or a development APK of the tree with `combat.force: true`: `combat_gunconnect_install` with `result: installed`;
offline Practice on a build where the local gun does not fire: `combat_gunconnect_gun_cs` with `unconnected` of 1 or more, then about 2 s after the combat tag appears one
`combat_gunconnect_connect` with `tries: 1`, then `combat_gunconnect_connected` within about a quarter second, and the gun fires. Online: `combat_gunconnect_gun_cs` with `unconnected: 0`
and no `connect`. Falsifiers: `connect` lines up to `tries: 10` then `gave_up` (the call is made and does not connect); no `gun_cs` line in a match (key or vptr wrong, or the tick not running);
only `waiting` lines (the local-member read, whose `+0x138` is the unverified one); a crash at the first call; an arena match with any `combat_gunconnect_*` acting line.
This row has not been run: no headset log shows the module acting.

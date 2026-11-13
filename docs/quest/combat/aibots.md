# Module `aibots` (practice AI bots get a scenario each combat match)

Files: `src/quest/combat/aibots.{h,cpp}`, `aibots_targets.h`, `netgame_targets.h`, `pinned_words.h`, `src/quest/tests/aibots_test.cpp`, `src/quest/combat/mutants.d/aibots.sh`. A normal
combat step: the combat APKs only. Switches in `config.yaml`: `combat.aibots` (on unless false) and `combat.aibots_scenario` (a text; unset is RandomAttack).

Shipped EC never starts an AI scenario, so the Practice AI bots stand still. `CR15NetAIScenarios::Update` does nothing until `EnableScenario` creates one, and its only caller is a script
node no Quest script contains. The module calls `EnableScenario(scenarios, state)` the way that node does, from the game tick.

## Origin

kotorvr's echoquest-combat repository (commit c5a67e8), `runtime/native/afkxr/afk_aibots.cpp`: the header :1-27, the constants :52-62, `ParseScenario` :158, `ReadRequest` :167 and
`afk_aibots_tick` :306-346 (the request protocol and the constants are the reference build's; `kEnableFirstWord` is at :61). The combat plugin imported in #1591 reconstructed it (`aibots.cpp`); this module is
that reconstruction, so the commits that brought it here credit kotorvr. Not ported: the watcher thread and its scan for the CR15Game (the tick passes it), the debug dump
and `aibots.done`.

## Behaviour

Looked at 4 Hz. In game = the net game exists, `net+8 == game` and the i32 at `net` is 9. Not in game: if a call was made, one `combat_aibots_rearmed` line, and the count is forgotten.
A combat match is the byte at `net+0x6341b` non-zero; the scenarios object is `net+0x2ab0`; the bot count is the u32 at `scenarios+0x500`. A count that is unchanged and zero, or already
called for, does nothing. A changed count is remembered with the time (`combat_aibots_bots`; a Warning when it is above 14 of the table's 16). Unchanged for 3 s: `EnableScenario(scenarios,
scenario)` once, again if the count later changes and holds; the record says whether the scenario object was set afterwards (`not_set` means the engine took the client path), whether the
engine's own host test passes (`flags & 2`, or `(flags & 0x44) == 0x40`) and whether a combat map is loaded. Ten seconds in a combat match (the game's combat flag set) with no call: one `combat_aibots_idle` line saying why. An arena match never writes it.

`combat.aibots_scenario` is read once at install: a number 0 to 6, or patrol, randomattack, weaponattack, spike, aithrowtest, aionevone, aibdebugarena; `off` is the module off. A word
that is none of them keeps RandomAttack with a Warning that does not name the word. In a development build the plugin's request file `<data dir>/aibots.req` is also polled at 1 Hz and
overrides both; a release build does not read it and warns when one is found.

## Arena

Arena and combat run on the same APK. In an arena match the bot count is zero, so the 4 Hz look ends after four reads, and the call is refused whatever the net game's flags say while the combat
level answer does not rule out an Echo Arena level (`combat_scope.h`, `SeenEchoArena`: its last look saw one, or the record is not fresh, which fails closed), with one
`combat_aibots_scoped_out` line per match whose `reason` is `echo_arena` or `no_fresh_look`. The module is one of the scope row's readers (combat_install.cpp `ScopeNormal`) and refuses its install
with `scope_not_live` when the answer cannot change; a scenario that is `off` in a release build is the module off and wins. `aibots_test` shows the refusal for the four arena levels, for a
transition holding an arena level beside a combat map, with no look and with a look a second old, and the call going through in a combat map, in the combat lobby and in the lobbies arena players
use (as before the move: those are not Echo Arena levels).

## Cost (ADR 0038)

With the switch off nothing is registered. With it on, the tick callback runs once per rendered frame (one clock read and one compare, about 8 ns on the x86 host that built this change). The
look is four process_vm_readv reads in an arena match (about 2 us, x86 host, 4 times a second) and six with bots in a combat match; the module reads no level itself: at the moment of a call it
reads the combat level answer's last look (a sequence-count read and a clock read), whose 4 Hz look is the scope row's, shared by every reader.
`aibots_test` prints the numbers. Not measured on a headset.

## Lines

`combat_aibots_install` (installed, off by `combat.aibots` or by the scenario, or refused with its reason at Error), `_scenario` (Warning), `_req`, `_bots`, `_agents` (Warning), `_enable`,
`_scoped_out` (with its `reason`), `_rearmed`, `_idle`, `_pinned_word` (Warning), `_legacy_switch` (Warning). The switch is also one `combat_config_switch` record.

## Pins

`aibots_targets.h` holds `EnableScenario`, `Scenarios()`, `ConnectBot`, `UpdateGameExpressionData` and 17 instruction words tied to the offsets by `static_assert`; the install checks the build
id and every word, and `Resolve` checks the first word of `EnableScenario` and refuses a name exported at another address. `got_pinned_test` checks the symbols and words against the store
libr15. `matchiscombat` is the game's own field: `Inspect<CBindingsOffsetOfInspector<bool>>` binds its record to `net+0x6340c+0xf` (0x2439df8, pinned).

## Headset row

Offline Practice AI on a combat APK: `combat_aibots_install` with `result: installed`; on entering the match `combat_aibots_bots` with `bots` N; 3 s after the count settles one
`combat_aibots_enable` with `scenario_name: randomattack`, `scenario_object: set`, `host: yes`, `combat_map: 1`; the bots move and fire instead of standing still. An arena match: no `enable` and no
`combat_aibots_idle`. One `combat_aibots_scoped_out` with `reason: echo_arena` appears only when the game's combat flag is set and an Echo Arena level is loaded
(a transition from a combat map into an arena level, with bots on the field): an arena match has its combat flag at 0, so the look ends before that line. An online combat match: no `enable`, and at most one
`combat_aibots_idle`. A stopped combat tick writes nothing here: the module's tick runs in the same loop as the scope row's refresh, right after it, and stops with it. So
`reason: no_fresh_look` is not expected on a headset: the record the module reads on the game thread, the thread that writes it, is at most 250 ms old;
a `no_fresh_look` line there means the module's row ran before the scope row's.
Falsifiers: bots stand still in a practice match with a `combat_aibots_idle` line (`scenarios_object` no means `net+0x2ab0`, `bots` 0 while bots are on the field means `scenarios+0x500`) or with none (the flag at `net+0x6341b`
did not read set); an `enable` with `scenario_object: not_set` or `host: no`; `combat_map: 0` in a combat level; a crash at the call.
This row has not been run: the headset logs that held the plugin's copy show its start line and one `idle` line from an arena match (`matchiscombat` 0, `bots` 0), never an enable; that line is no longer written there.

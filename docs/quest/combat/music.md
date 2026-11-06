# Module `music` (combat music, one instance at a time)

Files: `src/quest/combat/music.{h,cpp}`, `src/quest/combat/music_targets.h`, `src/quest/tests/music_test.cpp`, `src/quest/combat/legacy_switch.h`. A normal combat step, in the
`q15t` and `combat` APKs only. Switch in `config.yaml` (docs/adr/0046 rule 4): `combat.music`, true unless the file says false.

Pinned build: libr15 build id `b243509c08ce677aeb95fa348016949b3fc45230` (the store file; the running process checks the patched copy's id, `LibR15PatchedBuildId`).
ELF vaddr = disassembly VA - 0x100000.

## Provenance

The module is in kotorvr's (Ducky's) q15t-upstream: `runtime/native/afkxr/afk_music.cpp` at c5a67e8, whole file, and at the same addresses (`:51-52`: slot `0x36d76e8`,
function `0x2549264`). It came into this tree with the q15t plugin import (#1591) and moved here from `src/quest/plugins/q15t/src/music.{h,cpp}`. What is ours: the scope
(upstream acts on every Play), the typed thunk and the manager install, the 29 code-word pins (upstream pins none), the binary-search bank, and the log lines.

## Default

On. The seat brief's table of q15t-upstream defaults (`UPSTREAM-DEFAULTS.md`, row `music`) lists upstream's shipped build with it on (`afk_music.cpp:49`, `music.off` is the kill
file) and the plugin's `Q15T_MUSIC` was on. **No headset run of ours has shown it acting:** every q15t session in the integration runs (`run-quest2-2044`, `-handflip4`, `-sheet7`) logs the
module's install (`music on ... status=ok`) and no `music play` or `music post` line, because none reached a combat map; the one log with a combat match is the reference build's
(`run-quest2-sheet8`, `EchoQuestCombat: music: play b1b06cfb obj 8: stops first ...`), which is not this code. Its headset row therefore requires the acting lines below.

## What it does

Two combat-music Wwise trees start with a script Play (capture maps `2f22c384`, payload maps `b1b06cfb`); the only Stops of them
(`de586538`, `2acba759`) are posted by the PC's `nevt_return_to_lobby` listener. A match left any other way leaves the old instance
playing and the next match's Play starts a second one on top (tester report of overlapping music, source `:3-12`; source claim, not
re-verified here).

Wrapper on libr15's JUMP_SLOT for `CWwiseEngine::PostSoundEvent`:
- A Play event (`kMusicPlay`): first both Stops (`kMusicStop`, in that order) on the Play's own object, then both on the previous
  Play's object when it differs and is not the "none yet" sentinel `0xFFFFFFFF`, then the game's own Play with its own arguments
  (source `:96-114`). Logged: `music play: both music Stops posted first` with `event`, `obj`, `prev_obj` and the `stops` list (each
  `<stop id> obj <o> -> <rc> id <playing id>;`), then the post line.
- Every post of a bank event (24 ids: 2 Play, 2 Stop, 20 SetState) is logged once, after the real call: `music post` with `event`,
  `kind` (`play`/`stop`/`state`), `obj`, `rc` (low byte of the result), `playing_id` (read from the caller's id pointer with SafeRead,
  0 when none or unreadable) (source `:115-116`).
- Any other event: forwarded untouched, no work, no log, no counter (the hot path; `PostSoundEvent` is called for every sound).
- `MusicTick` (game thread, via the tick table, looks every 0.5 s): when a combat map was loaded at the last look and is not now, and a
  music Play was seen since the last such Stop, it posts both Stops once on the last Play's object with the Play's `self`, and logs
  `music: combat map gone, both music Stops posted` (source `:154-172`). "Combat map" is `CombatMapLoaded` (the four `mpl_combat_*`
  levels at `gs+0x58` in the two gamespace tables), which is the same walk as the source's `CombatLoaded` (`:128-146`; tables
  `game+0x1f48/0x1f78` and `game+0x1f88/0x1fb8`, `gs+0x8 == game`, 4 level hashes `:60-62`; compared by reading both, not by a diff).
- The line budget of the source (`kMaxLines = 4000` per process, `:59`) is kept, with one addition: the line that crosses it logs one
  WARNING (`music: line budget used up ...`), so a silenced log is never silent. Counters keep counting.
- Kill file: `<plugin data dir>/music.off` (`ModuleSwitchPresent`, `module_switch.h`, same directory as `fxtrail.off`) present at install:
  refused with reason `off_file` and one INFO line `music off`; posts are untouched. The source's `setprop debug.<app>.music 0` is
  dropped.

Logging (AGENTS.md): INFO `music on` (slot, real, status) at install; ERROR `music refused` / `music refused: code word pin mismatch`
on a refusal; INFO `music off`; INFO `music play`, `music post`, `music: combat map gone`; WARNING at the budget. No secrets.

## Arena (same APK)

Arena has its own music (`mp_arena_music`), which stays exactly as stock. PostSoundEvent is the game's one post for every sound event of every level, so:

1. The handler's first step is a binary search of the 24 bank ids (`Classify`). Any other event, which is every sound arena plays, goes to the game's function with the same
   registers; nothing is counted, posted or logged.
2. A bank event reaches code that changes anything only for the two Play ids, and only while the loaded levels hold a combat map and no Echo Arena level (`combat_level.h`,
   `Classify` with the lobby off; an arena level wins). The handler asks the level once per Play, not per post. No frame yet, an arena level, any lobby and no level
   pass the Play through and count it (`plays_out_of_scope`).
3. The unload tick looks at the levels only from an acted Play until the music is stopped; before that it is a published pointer and one load.

`music_test` `ArenaMusicIsExactlyStock` runs one stream (all 24 bank ids, forty ids of arena music's kind, and the odd registers) directly and through the installed module on an
arena level, the arena lobby, the social lobby and no level: the game sees the same calls in the same order with the same registers, answers the same, and no extra Stop is
posted by a Play or by the tick. `APlayOutsideACombatMapIsTheGamesCallAndNothingElse` does the same for the Play alone on ten level states.

## Cost (ADR 0038)

Host microbenchmark, x86-64 -O2 (`music_test`, built by hand at -O2 for the figure; the combat group builds at -O0; best of 7; machine load ~15; NOT a Quest number):

- a post outside the bank (what arena pays on every sound): the stand-in's own call is 2.5 ns; through the thunk plus the bank search it is 9.5 ns, **+7.0 ns** (the search alone is 4.4 ns).
- a bank post: one more relaxed add; a Play: the level scan (a few `process_vm_readv` reads; 3-6 us with one gamespace in `sensorwd_test`'s look) and up to four Stop posts.
- the tick callback, per frame: in arena one relaxed load of the published game and one acquire load of `g_playing`.

The calls per frame of PostSoundEvent are not measured (no headset run). At +7 ns, the 10 us-per-frame line of ADR 0038 would be reached at about 1,400 posts a frame in
arena; the plugin paid a 24-id linear scan on the same path.

## Lines

`combat_music_install` (state `on|off|refused`, reason `default|off_config|module_not_loaded|no_build_id|build_id_mismatch|pin_mismatch|tick_table_full|hook`, `status` the manager's),
`combat_music_first`, `combat_music_play` (the first 32), `combat_music_unload`, `combat_music_legacy_switch` (the plugin's `music.off` is not read; one found is a Warning). The handler
writes every line after the game's function has returned. The plugin's per-post line (`music post`, up to 4000 a process) is not kept: a post is counted, and `combat_music_unload` reports the counts.

## Headset row (owed)

On a combat APK built at a tag after this unit, or a development APK repacked with the `combat` variant word, one lobby, combat match, lobby round: the sentinel log holds
`combat_music_install` with `"state":"on"`, `combat_music_first`, `combat_music_play` with `"in_combat_map":1` and `"stops_posted":2` at each match start (`"prev_object"` the
previous Play's object on the second match), and `combat_music_unload` once within about half a second of the combat map unloading. An arena match on the same APK holds none of
the last three and the arena music plays as stock.

## Evidence (kotorvr's echoquest-combat music module and the game)

- Slot: `R0_readelf_r.txt:118641`: `00000000036d76e8  0001724500000402 R_AARCH64_JUMP_SLOT    0000000002549264 _ZN10NRadEngine12CWwiseEngine14PostSoundEventENS_12CSoundSymbolEjNS_7CFlagsTIjLy1EEEPFviPvES4_jRj + 0`.
  Source kotorvr's echoquest-combat music:51-52: `kGotPostSoundEvent = 0x36d76e8`, `kFnPostSoundEvent = 0x2549264`.
- Disassembly (read-only query): `functions`: `2649264 | PostSoundEvent | 564` (ELF 0x2549264 = VA - 0x100000).
- Bank ids: copied as literals from the source `:54-58` (2 Play `0x2f22c384 0xb1b06cfb`, 2 Stop `0xde586538 0x2acba759`, 20 SetState
  `0x045d3c23 ... 0xffae8834`); a script compared the 24 values in `src/quest/plugins/q15t/src/music.h` with those source lines: equal (24 = 24, same order).
  The test restates them independently.
- Argument layout, instructions of `PostSoundEvent` as read from the disassembly (VA, word):
```
2649264 ff8301d1 sub sp, sp, #0x60            2649298 f70305aa mov x23, x5   (cookie)
2649268 fb6b01a9 stp x27, x26, [sp, #0x10]    264929c f80304aa mov x24, x4   (callback)
264926c f96302a9 stp x25, x24, [sp, #0x20]    26492a0 f90303aa mov x25, x3
2649270 f75b03a9 stp x23, x22, [sp, #0x30]    26492a4 fa0300aa mov x26, x0   (self)
2649274 f55304a9 stp x21, x20, [sp, #0x40]    26492a8 e003142a mov w0, w20
2649278 f37b05a9 stp x19, x30, [sp, #0x50]    26492ac e10315aa mov x1, x21
264927c 680040b9 ldr w8, [x3]    (flags)      26492b0 e203082a mov w2, w8
2649280 e903022a mov w9, w2      (object)     26492b4 e30304aa mov x3, x4
2649284 5f040031 cmn w2, #0x1                 26492b8 e40305aa mov x4, x5
2649288 f40301aa mov x20, x1     (event)      26492bc e5031f2a mov w5, wzr
264928c 35119fda csinv x21, x9, xzr, ne       26492c0 e6031faa mov x6, xzr
2649290 f60307aa mov x22, x7     (&id)        26492c4 e7031f2a mov w7, wzr
2649294 f303062a mov w19, w6     (seek)       26492c8 8e081094 bl 0x02a4b500
26492cc c00200b9 str w0, [x22]   (*id = result)
26492d0 c0030034 cbz w0, 0x02649348
26492d4 730b0034 cbz w19, 0x02649440          (seek 0: no seek call)
```
  So: x0 `self`, x1 the event (only `w20` goes on), w2 the object (0xFFFFFFFF becomes all ones), **x3 is a pointer** to the flags word
  (`ldr w8,[x3]`), x4 callback, x5 cookie, w6 the seek in ms (`cbz w19` skips the seek call when 0), x7 a pointer that receives the
  result of the first engine call (`str w0,[x22]`, written unconditionally, so x7 must be valid: the extra Stops pass a stack word).
  The extra Stops therefore pass `(self, stop, object, &flags(=0), 0, 0, 0, &id)`, as the source does (`:83-88`).
  The 29 words 0x2649264..0x26492d4 above are the pins (`kMusicWordPins`, ELF = VA - 0x100000). A query built from the header's table
  returned `MATCH | 29 | 0 | 0` (29 pins, 0 not shown, 0 mismatched) against the disassembly.
- With seek 0 and a non-zero playing id the function returns `w22 = 0` (`2649440 mov w22, wzr` as the `cbz w19` target, then
  `2649444 mov w0, w22 ... 2649460 ret`); the other paths (id 0 at `0x2649348`, the seek call) I listed but did not analyse
  (unverified). The module treats the result as opaque and passes it through.

## Falsifiers

- `combat_music_install` with `"state":"refused"` on a libr15 the sentinel accepted by build id: `pin_mismatch` means the code at the pinned address is not what the argument layout was derived
  from; nothing is changed.
- No `combat_music_play` line in a combat match: the slot is not what the sound code calls on this build, the ids are wrong, or the level scan found no combat map when the Play was posted
  (the Play then counts as out of scope: `combat_music_play` still says `"in_combat_map":0`). Seeing only that line is the finding that the Play precedes the level in the gamespace tables.
- `combat_music_play` lines are present but the music still plays twice: the extra Stops do not end the instance (wrong object, or the Stop events need other arguments).
- A crash in the sound thread right after a `combat_music_play` line: the argument layout of the extra posts is wrong for the live engine.
- `combat_music_unload` never logged after a match although a Play acted: the tick is not registered, or the level hashes differ. Logged with the combat map still loaded, or music cut during a match.

## Not verified

All of the bank, the return-to-lobby listener and the claim that the two Stops end the trees are kotorvr's, read as text and not observed. The 24 ids are equal to upstream's (a script compared them),
the slot, the function and the 29 words were checked against the disassembly (`MATCH | 29 | 0 | 0`), and `got_pinned_test` (`just test-quest-hooks-pinned`, the store APK) checks the relocation and the words in the file.

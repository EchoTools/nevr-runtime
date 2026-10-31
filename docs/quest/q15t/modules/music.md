# Module `music` (source ticket T-48; no R-id)

Files: `src/music.{h,cpp}`, `src/quest/tests/q15t/music_test.cpp`, `src/quest/tests/q15t/mod/music.sh`, `src/quest/tests/q15t/mutants.d/music.sh`.

`R1_functions.md` has no row for `PostSoundEvent` (searched `music` and `PostSoundEvent` in the source-build function note, not in this tree: no match), so this
note has no R-id; the unit is the source build's `the source build's music` ("the source build" below, read in full).

## Wiring for the main seat

- Option macro `Q15T_MUSIC`, default **ON** (the source build installs it unless switched off: the source build's music:1 "ON by default",
  the source build's music:211 `Off()` check in `source-build music_install`).
- API (`src/quest/plugins/q15t/src/music.h`): `InstallResult InstallMusic(std::uintptr_t base)`, `void MusicTick(std::uintptr_t game)` (a `TickFn`), seams
  `InstallMusicAt(base, slot, real)`, `GetMusicStats()`, `ResetMusicForTest()`, `ResetMusicScheduleForTest()`, `MusicForceStateForTest()`.
- Install call, before `InstallGameTick` (every `AddGameTick` comes before it) and after the module's `ReportModule`:
  ```
  #if Q15T_MUSIC
    { const q15t::InstallResult r = InstallMusic(base); ReportModule("music", r);
      if (r.status == q15t::SwapStatus::kOk) AddModuleTick("music", &q15t::MusicTick, &want_tick); }
  #endif
  ```
  The module does **not** register its tick itself. A tick registered although the install was refused is harmless (it only acts after a
  Play seen by the wrapper), but the line above registers it only on success. One more entry in the 16-slot tick table.
- `CMakeLists.txt`: add `src/quest/plugins/q15t/src/music.cpp` to the plugin sources, `option(Q15T_MUSIC ... ON)`, `MUSIC` in the `foreach(m ...)` list and
  `Q15T_MUSIC=$<BOOL:${Q15T_MUSIC}>` in the definitions. `music.cpp` calls `CombatMapLoaded` from `src/quest/plugins/q15t/src/refresh_rate.cpp` (already in
  the plugin) and uses the header-only `module_switch.h`; no other new dependency.
- Capability: not `ALTERS_GAMEPLAY` (it only posts audio events; my judgement, same class as `impactsnd` in the comment in plugin.cpp).
- `Report()` string: `music`. The kill file makes the module report a non-ok status with reason `off_file`.
- Host test and mutants are picked up by the existing loops (`src/quest/tests/q15t/mod/*.sh`, `src/quest/tests/q15t/mutants.d/*.sh`).

## What it does (source the source build's music:1-34)

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

## Mechanism and gates

`InstallImportHook(symbol, base, 0x36d76e8, &PostWrapper, &g_real, 0x2549264)`: a data-page swap through `got_swap` (which refuses an
executable page), the slot must hold exactly `base + 0x2549264`. No code write, no `PROT_EXEC`, no constructor
(`src/quest/tests/q15t/mod/music.sh` cross-compiles `src/quest/plugins/q15t/src/music.cpp` with the NDK r26d clang and fails on `.init_array`/`.ctors`).

Gate order: build id (plugin `InitEx`, not re-checked) -> 29 code-word pins read with SafeRead at `base + ELF vaddr` before anything is
touched (`word_pin_mismatch`, one ERROR naming `pin=0x<ELF vaddr> found=0x<word or unreadable> expected=0x<word>`) -> `music.off` ->
slot-target check (`unexpected_original`, one ERROR naming `pin=<slot ELF vaddr> found=<word at the slot> expected=<base + real>`).
**There is no MD5 and no whole-file hash.**

No chaining. The source accepts a slot that holds "another wrapper in our own library" (its `source-build audio` event tap, `:28-30`,
`Ours()` `:202-206`) and calls that one as the real function. `MODULE_CONTRACT.md` (read in full) has no slot-ownership or chaining
rule; `rg 36d76e8 src docs` found nothing before this module (our plugin has no other user of this slot), and `InstallImportHook` with
an expected vaddr accepts only the stock function. So any other word is refused, loudly, slot untouched (host test: a foreign word, and
a word that points at real executable code). If a second wrapper on this slot is ever added, it needs its own design and its own tests.

## Evidence

- Slot: `R0_readelf_r.txt:118641`: `00000000036d76e8  0001724500000402 R_AARCH64_JUMP_SLOT    0000000002549264 _ZN10NRadEngine12CWwiseEngine14PostSoundEventENS_12CSoundSymbolEjNS_7CFlagsTIjLy1EEEPFviPvES4_jRj + 0`.
  Source the source build's music:51-52: `kGotPostSoundEvent = 0x36d76e8`, `kFnPostSoundEvent = 0x2549264`.
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

## Differences from the source build

- No `setprop` switch, no `music.off` in the source build's data directory under `/sdcard/Android/media/...`: the kill file is `<plugin data dir>/music.off`.
- No chaining (see above). Code-word pins are an addition (the source pins none for this module).
- Plain loads become SafeReads (the caller's id pointer; the gamespace walk was already SafeRead in the source, `source-build safe_read`).
- Log lines are structured (fields) instead of one formatted string; the line budget now warns once.
- The source's `Tick` posts to `g_lastObj` even when it is the sentinel `0xFFFFFFFF`; the wrapper skips that sentinel as a target
  (`:102`). Both kept as in the source (a Play on game object 0xFFFFFFFF is not expected; unverified that it never happens).
- The source logs "NO exclusive tick" when `source-build exclusive_tick_add` fails (`:233-237`); here the main seat's `AddModuleTick` reports a
  full tick table as an ERROR naming the module.

## Prediction (written before any headset run)

On the pinned build, module on, in a lobby -> combat match -> lobby round: the log shows `music on: ... status=ok` at start. At each
match start `music play: both music Stops posted first event=2f22c384` (capture) or `b1b06cfb` (payload) with
`prev_obj=4294967295` the first time and the previous object afterwards, `stops=` four or two entries, then ~10-20 `music post` lines
(SetState and the Play) per match. When the match is left, `music: combat map gone, both music Stops posted obj=<the Play's object>`
appears once within ~0.5 s of the combat map unloading (a normal match end: after the script's own Stop, so both Stops are no-ops).
Music no longer plays twice after leaving a match through the menu's return to lobby. Nothing else about the audio changes.

## Falsifiers

- `music refused: code word pin mismatch` (ERROR; pin / found / expected named) on the build the plugin accepted by build id: the code
  at that address is not what the layout was derived from; nothing is swapped, the game runs stock.
- `music refused ... status=unexpected_original` (pin / found / expected named): the slot holds another wrapper or a different build.
- No `music post` line in a combat match: the slot is not what the sound code calls on this build (or the ids are wrong).
- `music play` lines are present but music still plays twice: the extra Stops do not end the instance (wrong object, or the Stop events
  need other arguments); the source's claim that a Stop of nothing is a no-op and that these two events stop the tree is unverified here.
- A crash in the sound thread right after a `music play` line: the argument layout of the extra posts is wrong for the live engine.
- `music: combat map gone` never logged after a match although a Play was: the tick is not registered, or the level hashes differ.
- `music: combat map gone` logged with the combat map still loaded, or music cut during a match.

## Needs a word patch / not built

- **Needs a word patch:** none (data-page swap only).
- Not built: the chain with another wrapper; the `setprop` switch; the md5 gate (deliberately none).
- Unverified: all source-build claims about the bank, the PC's `nevt_return_to_lobby` Stop listener, that the two Stops end the
  trees, the callers of `PostSoundEvent` (`CSoundPlayer::Play3DSound` / `PlayQueuedOneOffs`, source `:14-15`), thread safety of posting
  from the game thread tick (the source does the same), and the cost of the wrapper per call (a table scan of 24 ids; not measured).
  I verified the slot, the function name and size, the 29 words and the argument layout above.

## Tests

`src/quest/tests/q15t/music_test.cpp` (real code, fake slot page, fake `PostSoundEvent` recording every call's registers, fake CR15Game table, fake
game image with the pinned words at the real ELF vaddrs): Stops precede each Play (order, objects, flags 0, no callback/cookie/seek, own
id pointer that starts at 0), the game's own arguments and result untouched, no Stop when the event is not a Play, 24 bank ids logged
with kind/obj/rc/id, nine non-bank ids (near misses of the bank ids, 0 and 0xffffffff) silent, high bits of the event register, unreadable/absent id pointer, line budget,
tick (once, on the right object and self, not before a Play, not without having seen a combat map, 0.5 s period, re-arm by a new Play,
no real function), refusals (foreign word, other executable function, null base, kill file, other module's kill file, full installer on
an image: wrong build, foreign slot, kill file, unreadable library, each with no write), 29-pin table fingerprint and every pin flipped.
`src/quest/tests/q15t/mutants.d/music.sh`: 105 mutants, all killed by a failing check; none is a compile error (checked: every mutant compiled
with the test's own flags before being counted).

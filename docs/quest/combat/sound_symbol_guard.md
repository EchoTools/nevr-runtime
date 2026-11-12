# Module `sound_symbol_guard` (#1737)

Files: `src/quest/combat/sound_symbol_guard.{h,cpp}`, `src/quest/combat/sound_symbol_guard_targets.h` (the pins and the measurements this
module stands on), `src/runtime/combat/sound_symbol_guard_rules.h` (the rule, one definition for PC and Quest),
`src/quest/tests/sound_symbol_guard_test.cpp`, `src/quest/tests/sound_symbol_guard_pinned_test.cpp`, `src/quest/combat/mutants.d/sound_symbol_guard.sh`.
A normal-phase row of the combat install table (`src/quest/combat/combat_install.h`), directly after `bullet_velocity_guard`'s: it runs in the
combat APKs and in no other.

## Origin

The fxsndguard switch of kotorvr's echoquest-combat: runtime/native/afkxr/afk_fx.cpp, its sound guards (the lines are named in
`sound_symbol_guard_targets.h`), read as text. What is taken is the behaviour (a sound post whose symbol cannot play is not made). The symbol list,
the call-site gate, the drop value and the scope are this tree's, from the measurements in `sound_symbol_guard_targets.h`'s header comment.
Upstream's third value, `0x0f986c58c7b3d4fb`, is the error record's type key that PostSoundEvent passes to CErrMsg::CreateAndAddError, not a
sound symbol, so neither it nor its low word `0xc7b3d4fb` is listed.

## Switch and default

`combat.sound_symbol_guard` in `config.yaml`, a boolean, default **false**. A value that is not a boolean keeps the default, with a Warning that
names the key.

Why off. The guard's default was to follow whether a dropped post's alternative can fault. It can in the code: OutputError, which Play3DSound
calls with PostSoundEvent's status, has a path that calls through a null record when the record at that status is incomplete. But it is not shown
to be reached on this path: whether CErrorDb::AddError (whose status CreateAndAddError returns unchanged, its reconstruction) ever answers a status whose record is incomplete, and whether the CConsoleStream
constructor (upstream's crash frame, return 0x11d1cac) can fault, are open (neither function is reconstructed yet). With no fault shown the guard
is a performance guard: it saves a failed post, its error record and OutputError's callstack and console write, which upstream measured at about
4 ms a print and 230 posts a second for symbol 0. Upstream ships it off too (`fxsndguard.on` is not among the recipe's switch files). A player
opts in with `sound_symbol_guard: true`.

## What it hooks

`NRadEngine::CSoundPlayer::Play3DSound(CSoundSymbol, unsigned, float)`, JUMP_SLOT ELF 0x3701090 -> 0x134b468 (`sound_symbol_guard_targets.h`
`kPins.play3DSlot`, `play3DFn`), through a `CallbackThunk` that hands the handler the call's return address (`kCaller`).

Why that slot and not PlaySound's two words. PlaySound calls Play3DSound through the PLT stub at ELF 0xf53350 (`bl` at 0x1706988), and the
GLOB_DAT word `kPins.playDataSlot` is a pointer to PlaySound itself, which the deferred queue calls; the bullet site (`bl` at 0x21230d0) uses the
same stub. So every post from PlaySound, direct or deferred, and every bullet post pass Play3DSound's one slot, and no data word is written.
The stub ends in `br x17`, which leaves x30 alone, so the return address the thunk reads is the call site's. `kPins.playSlot` and
`playDataSlot` stay pinned and unhooked.

The rule (`nevr_combat::sound_symbol_guard::Classify` on the low 32 bits of x1, the CSoundSymbol Play3DSound posts): class `zero` for 0 (Wwise's
invalid id), class `unregistered` for a symbol of `kUnregistered[]`, today only `0x1406b30a`, the bullet site's default (`_INIT_355` writes it to
the .bss word 0x37af76c at load). tools/cmd/wwise-events over the two stock packages of the store OBB found 34 banks and 1295 event ids, and
`0x1406b30a` is no event and no HIRC object of any type in them. A symbol is added to the list only on such a measurement.

The gate. A post is dropped only when all three hold:

1. A combat map or the combat lobby is loaded (`scope::SeenCombatOrLobby()`, then `scope::LookIsFresh()`).
2. Its symbol is classed.
3. It returns to PlaySound (ELF 0x170698c) or to the bullet site (0x21230d4). Both post on an emitter made by CreateEmitter(name, 0, 0), so bit 0
   of emitter+0xbc ("destroy the emitter when its sound does not start") is clear and a failed post would destroy nothing: dropping it leaves
   nothing undone. CRxMaterialFXCS::TriggerSingle posts on an emitter made by CreateEmitter(name, 1, 0): a drop there would skip the destroy its
   failed post makes and could leak the emitter until CSoundPlayer::Shutdown, so its posts, and those of the six other callers whose emitters are
   not established, reach Play3DSound whatever the symbol (counted `kept_other_caller`).

A dropped post returns 0 without calling Play3DSound: Play3DSound's own answer when audio is disabled (`mov w0, wzr` at 0x134b488). PlaySound
branches on the answer with `cmn w0, #1; b.ne` at 0x1706990: the branch is taken for any answer but -1 and goes to PlaySound's epilogue
(0x1706a04), and the fall-through for -1 loads the string "Sound failed to play from node %llx in %s." (.rodata 0x2c4e2dc). That PlaySound
therefore treats 0 as a start and prints nothing is read from those words; it is inferred, because PlaySound is not reconstructed (reconstruction
pending). The same inference covers what a 0 does to PlaySound's returned handle: its playing id is then 0, Wwise's invalid id, where a failed
post would give -1 (`bfi x8, x0, #32` at 0x1706a14), the same value the game already returns when audio is disabled. The bullet site ignores the
answer. Returning -1 would make PlaySound print "Sound failed to play" for every drop. Every post that is not dropped reaches Play3DSound with
the same four arguments (the whole x1 register included), and its answer is returned.

## Log lines (sentinel log)

- `combat_sound_symbol_guard_install`: `result` (`installed|off|refused`), `reason` (`ok`, `off_config`, `scope_not_live`, `module_not_loaded`,
  `no_build_id`, `build_id_mismatch`, `pinned_word_mismatch`, or the manager's status by name: `original_mismatch`, ...), `slot`, `msg`. Info for
  installed and off, Error for a refusal; `pinned_word_mismatch` adds `pin`, `vaddr`, `found` (`unreadable` for a page that cannot be read) and
  `expected`. The install checks every word and record of `kWordPins` and `kDropPins` before it touches the slot.
- `combat_sound_symbol_guard_drop`: Debug, the first 16 drops of the process: `symbol` (hex), `class` (`zero|unregistered`), `caller`
  (`play_sound|bullet_site`), `n` (the drop's number in its class), `msg`.
- `combat_sound_symbol_guard_drops`: Debug, once at the 17th drop with `since_last` 0 (later ones are counted, not logged), then at most every
  10 s while drops continue, with `n` and `since_last` (`nevr::BurstThenCount`, `src/core/burst_then_count.h`).
- `combat_sound_symbol_guard_total`: Info at the end of the process, only when installed: `dropped_zero`, `dropped_unregistered`,
  `kept_other_caller`, `calls` (every post seen in a combat level).

## Arena cost and arena behaviour (arena and combat share the APK)

- **Behaviour.** README rule 9: outside a combat map or the combat lobby the handler tail-calls Play3DSound with the same arguments and does nothing
  else (no counter, no read, no line); `sound_symbol_guard_test` (`OutsideACombatLevelEveryPostPassesUntouched`) pins it in an arena level, with
  no level, with an arena level beside a combat one and on a stale answer, by count (no post counted, no level scan).
- **Cost.** Per post outside a combat level: the thunk's entry (its call counter and two loads) and the handler's load and test of the scope
  word. The host test prints the cost against the stand-in called directly. In a combat level: a clock read, a relaxed increment and store, the
  class test, and for a classed symbol two compares. Not measured on a headset.

## Not shown

- That OutputError's null-record path, or the CConsoleStream constructor, faults on this path (so whether the guard is also a crash guard).
- Which emitters the six other callers of Play3DSound's stub post on (Play2DSound, CSoundVolume::PlayAmbientSounds,
  CAmbientSoundCS::PlayAmbientSound, CAmbientSoundCS::SendEvent, PlaySoundAtAttach, CSendAmbientSoundActionNode::Enter): they are never dropped.
- That no bank loads from outside the two stock packages, and the run-time rate at which symbol 0 and the default reach the two sites.
- That no other writer stores PlaySound's emitter word or the bullet emitter table (the whole-function reconstructions are requested).

## Headset row

Quest 3, combat APK with `sound_symbol_guard: true`: a 10-minute Gauss combat match with no SIGSEGV in OutputError,
`combat_sound_symbol_guard_install` `installed` at start, `combat_sound_symbol_guard_total` at the end with its counts; then an arena match:
the total's `calls` does not grow during it (expected 0 drops in arena).

## Platform

PC: not applicable until shown. echovr.exe's PlaySound (VA 0x1403f94c0) calls OutputError (0x14151ee70) at 0x14020b8c1 after PostSoundEvent with no
test of the symbol for 0, but whether that call reaches the console-stream path is not established; #2375 carries it as a second falsifiable
item beside the PC bullet-velocity guard. The rule header is shared.

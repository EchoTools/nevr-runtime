# sendguard: a script send to a game space that is not loaded is dropped, not faulted

Source: `src/quest/combat/sendguard.{h,cpp}`, targets `sendguard_targets.h`. Namespace `nevr_quest::combat::sendguard`, log events
`combat_sendguard_*`. Switch: `combat.sendguard` in `config.yaml` (default `true`). Runs where `combat::Active()` is true (the q15t and
combat APK variants; `src/quest/combat/combat_active.h`). Host test: `src/quest/tests/sendguard_test.cpp` (`just test-quest-combat
sendguard`); mutants: `src/quest/combat/mutants.d/sendguard.sh`.

Origin: written for this project from the game's code (below). The reference build and EchoCombat have no counterpart, and
`docs/audits/2026-11-05-q15t-origin-citations.md` lists no origin token for it; it came with the q15t plugin that `ef714347` imported.

## What the stock code does

Disassembly, libr15 (ELF vaddr = disassembly VA - 0x100000), build b243509c08ce677aeb95fa348016949b3fc45230 (store file sha256
`8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20`):

- `NRadEngine::CSendEventNode::Enter`, ELF `0x2725a80`, 476 bytes (`.dynsym` FUNC). `x0` is the node's execution context, `x1` the node data
  (`data[0]` the level key, `data[1]` the event, `data[2]` the actor).
- `0x2725a90..0x2725ab0` reads the current game space (`ldr x19,[x9,#0x80]`). When `data[0] != -1` (`cmn x1,#1` at `0x2725aa4`),
  `0x2725abc` replaces it with `CncaGameSpace::GameSpaceMT(space, data[0])`, through the PLT stub at `0xef5650` (`readelf -rW`: the stub's
  JUMP_SLOT, `0x36d2210`, names `_ZNK10NRadEngine13CncaGameSpace11GameSpaceMTENS_9CSymbol64E`, defined in libr15 at `0x1806704`).
- The actor lookup at `0x2725af4` (`SComponentNodeUtils::Actor`) is given the first space, not the looked-up one. It succeeds while the
  named level is absent.
- `0x2725b78` is `ldr x9,[x19,#0x58]`. With the named level absent `x19` is 0 and the load faults: the SIGSEGV seen on a headset with the
  menu gate on (`libr15 ... CSendEventNode::Enter, offset 0x2725b78 (ELF), x19=0`).
- The engine's other callers of `GameSpaceMT` test the result for zero (`PlayAnimation` `0x146c5c4`, `CanPlayAnimation` `0x146e2b4`,
  `UpdateBindings` `0x1490b6c`). `Enter` does not.
- One data word is a relocation that names `Enter`: the JUMP_SLOT at ELF `0x36f7928` (`readelf -rW` on the store file lists exactly this one for
  the symbol). The disassembly's data cross-references list two more words, and neither is a hook target: `0x356a6b0` is the eight bytes
  `d0b31bff dc010000` inside `.eh_frame` (`0x330ed50` to `0x35f7e10`), an FDE's `pc_begin` (`0x356a6b0` plus the int32 `0xff1bb3d0` is
  `0x2725a80`) and its range `0x1dc`, the function's size: its unwind record, not a pointer, and no relocation names it. `0x33e7924` is
  4-aligned and cannot hold a pointer word.

## What the module does

- Reads the build ID of the loaded libr15 and the ten code pins of `sendguard_targets.h` (thirteen words: the reads the wrapper repeats,
  the faulting load and the four words of the `GameSpaceMT` stub) before the slot is touched. A mismatch refuses the module with one Error
  line that names the pin, and changes nothing.
- Installs one thunk on the JUMP_SLOT with `InstallThunk`, checked against `base + 0x2725a80`. A slot that holds anything else is refused
  by the hook manager and logged with its chain.
- The handler repeats the stock function's own reads, calls the stub for `GameSpaceMT(space, data[0])`, and returns without sending when the
  answer is null. Otherwise it calls the stock function, which asks again (a lookup with no side effect). A null link on the chain, a null
  context or a null node is left to the stock function.
- With `combat.sendguard: false` it installs nothing and says so at Info (`reason: off_config`). The old plugin's `sendguard.off` file is not
  read; one found in `/sdcard/Android/data/com.readyatdawn.r15/files/q15t` is a Warning that names the key.

Which level the node names, and whether it is combat content, is not shown by the reconstruction; the first eight drops log `level_key`,
`event_key` and `actor_key` so a headset run names them.

## Logging

All lines are in the sentinel log, one JSON object each:

- `combat_sendguard_install`: `result` `installed` (Info, with `hook: ok`), `off` (Info, `reason: off_config`) or `refused` (Error, with `why`
  `module_not_loaded`, `no_build_id`, `build_id_mismatch` or `pin_mismatch` and the pin's `pin` name, or `hook` set to the hook manager's status),
  and the consequence in words.
- `combat_sendguard_dropped`: Info, the first eight in the process, with `n`, `level_key`, `event_key`, `actor_key`. Every drop is counted.
- `combat_sendguard_legacy_switch`: Warning, when `sendguard.off` is found.
- `combat_install` (the combat area's line for the normal phase) carries `sendguard=<status>` in its `results`, and `combat_config` the
  switch's value and source.

## Headset check

Log only; no player-visible change. On a combat APK, `combat_sendguard_install` with `"result":"installed"` and no Warning in the same
run; with the menu gate on and no combat content pack, pressing PLAY gives no SIGSEGV and the lobby loads, and `combat_sendguard_dropped`
names the level key the menu addressed. Not run: no headset.

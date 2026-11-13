# sendguard: a script send to a game space that is not loaded is dropped, not faulted

Source: `src/quest/combat/sendguard.{h,cpp}`, targets `sendguard_targets.h`. Namespace `nevr_quest::combat::sendguard`, log events
`combat_sendguard_*`. Switch: `combat.sendguard` in `config.yaml` (default `true`). Runs where `combat::Active()` is true (the combat
variants; `src/quest/combat/combat_active.h`). Host test: `src/quest/tests/sendguard_test.cpp` (`just test-quest-combat
sendguard`); mutants: `src/quest/combat/mutants.d/sendguard.sh`.

Origin: written for this project (#1604) from the game's code (below). The reference build and EchoCombat have no counterpart, and
`docs/audits/2026-11-05-q15t-origin-citations.md` lists no origin token for it.

## What the stock code does

Disassembly, libr15 (ELF vaddr = disassembly VA - 0x100000), build b243509c08ce677aeb95fa348016949b3fc45230 (store file sha256
`8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20`):

- `NRadEngine::CSendEventNode::Enter`, ELF `0x2725a80`, 476 bytes (`.dynsym` FUNC), is the script node that sends a component event to a game
  space (`CncaGameSpace::SendComponentEvent`, `0x2725b90`): the script-event path inside the game, not the network send path. `x0` is the node's execution context, `x1` the node data
  (`data[0]` the level key, `data[1]` the event, `data[2]` the actor).
- `0x2725a90..0x2725ab0` reads the current game space (`ldr x19,[x9,#0x80]`). When `data[0] != -1` (`cmn x1,#1` at `0x2725aa4`),
  `0x2725abc` replaces it with `CncaGameSpace::GameSpaceMT(space, data[0])`, through the PLT stub at `0xef5650` (`readelf -rW`: the stub's
  JUMP_SLOT, `0x36d2210`, names `_ZNK10NRadEngine13CncaGameSpace11GameSpaceMTENS_9CSymbol64E`, defined in libr15 at `0x1806704`).
- The actor lookup at `0x2725af4` (`SComponentNodeUtils::Actor`) is given the first space, not the looked-up one. It succeeds while the
  named level is absent. When it fails the stock function reports `ScriptActorError` (`0x2725b48`) and returns without faulting, so the
  fault needs the level absent and the actor found.
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
  answer is null. Otherwise it calls the stock function, which asks again (the lookup is idempotent: it may sort the game's space table once
  after a change, and the stock call does the same). A null link on the chain, a null context or a null node is left to the stock function.
- What else it drops: a send whose level is absent and whose actor would not resolve either. The stock function would have reported
  `ScriptActorError`; the guard returns without the report. This is logged, not passed through: each of the first eight drop lines says the
  stock function "would fault, or report a missing actor, which is now not reported". Passing that case on would mean evaluating the actor
  lookup, which is game code with effects of its own.
- With `combat.sendguard: false` it installs nothing and says so at Info (`reason: off_config`); the phase line counts it as `off`, neither
  installed nor failed. The old plugin's `sendguard.off` file is not read; one found in the data directory below the sentinel's files folder
  (`legacy_switch.h` `kDataSubdir` below `sentinel::FilesDir()`, the same folder the sentinel's config and log use) is a Warning that names the key.
- Counts: `passed` (sends that reached the game's function) and `dropped` are kept. A Debug `combat_sendguard_stats` line is written every
  16384 passes and every 1024 drops after the eighth, and one Info line at the end of the process (`final: 1`, when the wrapper is installed),
  so a headset run gives the call rate and the drops after the eighth leave a trace.

Which level the node names, and whether it is combat content, is not shown by the reconstruction; the first eight drops log `level_key`,
`event_key` and `actor_key` so a headset run names them.

## Logging

All lines are in the sentinel log, one JSON object each:

- `combat_sendguard_install`: `result` `installed` (Info, with `hook: ok`), `off` (Info, `reason: off_config`) or `refused` (Error, with `why`
  `module_not_loaded`, `no_build_id`, `build_id_mismatch` or `pin_mismatch` and the pin's `pin` name, or `hook` set to the hook manager's status),
  and the consequence in words.
- `combat_sendguard_dropped`: Info, the first eight in the process, with `n`, `level_key`, `event_key`, `actor_key`. Every drop is counted.
- `combat_sendguard_stats`: `passed`, `dropped`, `final`. Debug while the process runs, Info once at its end.
- `combat_sendguard_legacy_switch`: Warning, when `sendguard.off` is found.
- `combat_install_module` (phase `normal`, `module` `sendguard`) carries its `outcome` and `status`, and `combat_install` (the combat area's line for
  the normal phase) counts it in `installed`, `failed` or `off`. The switch is reported by its own `combat_config_switch` record, `{"module":"sendguard","key":"combat.sendguard","on":0|1,
  "source":"config|default"}`, generated from the module's table row (`Module::switchKey`), not by a field of the `combat_config` line.

## Arena cost

It is installed in an arena match on the combat APK, because the gate is per APK. It acts in every level on purpose: the fault it guards
happened in the lobby (a menu script's event to a combat sub-level that a Quest without the combat content pack never loaded), so a scope by
level would leave it out. What an arena player sees changes only where the game would otherwise crash (the send is dropped), plus the
missing-actor report named above. Cost per call, counted from the arm64 code and not timed: two atomic increments (the thunk's call counter
and `passed`) and two indirect branches; a call that names a level adds five dependent loads and one `GameSpaceMT` lookup. How often `Enter`
runs is not known; the stats lines are how a headset run measures it.

## Headset check

Log only; no player-visible change. On a combat APK, `combat_sendguard_install` with `"result":"installed"` and no Warning in the same
run; with the menu gate on and no combat content pack, pressing PLAY gives no SIGSEGV and the lobby loads, and `combat_sendguard_dropped`
names the level key the menu addressed. Not run: no headset.

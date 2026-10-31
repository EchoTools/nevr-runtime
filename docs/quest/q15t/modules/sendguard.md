# sendguard: a script send to a game space that is not loaded is dropped, not faulted

Source: `src/quest/plugins/q15t/src/sendguard.cpp`. Option: `Q15T_SENDGUARD` (on by default in `CMakeLists.txt`). Off file:
`sendguard.off` in the plugin data directory. No host test yet (KNOWN-WRONG #1603: the module ships without one; the headset check below is
the only run).

## What the stock code does

Disassembly, libr15 (ELF vaddr = disassembly VA - 0x100000), build b243509c08ce677aeb95fa348016949b3fc45230:

- `NRadEngine::CSendEventNode::Enter`, disassembly `0x2825a80`, 476 bytes (raw decompilation and disassembly read in the disassembly this
  session). `x0` is the node's execution context, `x1` the node data (`data[0]` the level key, `data[1]` the event, `data[2]` the actor).
- `0x2825a90..0x2825ab0` reads the current game space (`ldr x19,[x9,#0x80]`). When `data[0] != -1` (`cmn x1,#1` at `0x2825aa4`),
  `0x2825abc` replaces it with `CncaGameSpace::GameSpaceMT(space, data[0])` (the stub at `0xff5650`).
- The actor lookup at `0x2825af4` (`SComponentNodeUtils::Actor`) is given the first space, not the looked-up one. It succeeds while
  the named level is absent (`cbz w0` at `0x2825af8` falls through).
- `0x2825b78` is `ldr x9,[x19,#0x58]`. With the named level absent `x19` is 0 and the load faults. This is the SIGSEGV seen on the
  headset with the menu gate on: `libr15 ... CSendEventNode::Enter, offset 0x2725b78 (ELF), x19=0`.
- The engine's other callers of `GameSpaceMT` test the result for zero (`PlayAnimation` `0x146c5c4` and `0x1470004`,
  `CanPlayAnimation` `0x146e2b4`, `UpdateBindings` `0x1490b6c`). `Enter` does not.
- Two data words hold the address of `Enter` (disassembly xrefs to `0x2825a80`, `[data]`): `0x366a6b0` and `0x37f7928` (the GOT slot of the
  stub at `0x1040480`); ELF `0x356a6b0` and `0x36f7928`. A third, `0x33e7924`, is 4-aligned and is not swapped. Which words the node
  dispatch reads is not shown; both are swapped.

## What the module does

- Pins the 13 code words listed in `sendguard.cpp` (the reads it repeats, the faulting load, the `GameSpaceMT` stub) before any slot is
  touched. A mismatch refuses the module with one ERROR line.
- Swaps each slot with `InstallImportHook`, each checked against `base + 0x2725a80`.
- The wrapper repeats the stock function's own reads, calls the stub for `GameSpaceMT(space, data[0])`, and returns without sending
  when the result is null. Otherwise it calls the stock function.

Which level the node names, and whether it is combat content, is not shown by the reconstruction; the first eight drops log `level_key`,
`event_key` and `actor_key` so a headset run names them.

## Logging

`sendguard: slot swapped` (INFO, per slot), `sendguard: slot refused, its calls stay stock` (WARNING), `sendguard: on` (INFO, `slots`,
`of`), `sendguard: OFF; no slot installed` (ERROR), and `sendguard: send dropped, the game space the node names is not loaded`
(INFO, the first eight, with `n`, `slot`, `level_key`, `event_key`, `actor_key`).

## Headset check (3 lines)

1. `logcat | grep 'sendguard:'`: expect `slot swapped` for each slot and `sendguard: on`.
2. Press PLAY on the main menu with the menu gate on: expect no SIGSEGV and the lobby loads.
3. Note the `send dropped` lines' `level_key` (the level the gate-open menu addressed) and whether the combat area is open.

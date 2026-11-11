# Module `hudmeter` (row R57): the wrist HUD's weapon-slot heat fill

Files: `src/quest/combat/hudmeter.{h,cpp}`, `src/quest/combat/hudmeter_targets.h` (the pins), `src/quest/combat/combat_scope.{h,cpp}` (the level
answer the handler reads), `src/quest/tests/hudmeter_test.cpp`, `src/quest/tests/got_pinned_test.cpp`, `src/quest/combat/mutants.d/hudmeter.sh`. The module
is a normal combat step, registered after `refresh_rate` (`src/quest/combat/combat_install.cpp`): it runs in the `q15t` and `combat` APKs and in no
other. Switch: `combat.hudmeter` (default true) in `config.yaml`; false installs nothing and logs `combat_hudmeter_install` with `reason: off_config`.
No file in the app's files directory turns it on or off.

Origins: our implementation of the reference build's `runtime/native/afkxr/afk_hudmeter.cpp` (the zero-seed trap :1-9, the offsets and the buffer
:43-48, the switch :49, the slot and function :76, the eleven pinned code words :83; pinned in `docs/audits/2026-11-05-q15t-origin-citations.md`, rows
75-84). The module was imported into the q15t plugin from kotorvr's echoquest-combat work (#1591) and moved here. The reference build's rev 4 "old bar"
part (its `oldbar.on` and `oldbarfix.on` TEST switches, which mirror another element's writes across actors) is not built.

## Row covered

R57: `NRadEngine::CSetCanvasElementAttributesNode::Enter(SSetCanvasElementAttributesNodeData const&)`, JUMP_SLOT ELF 0x36c6ce8, real function ELF 0x2726eac
(disassembly 0x2826eac, size 1600). `readelf -rW` of the store libr15 (sha256 `8dd9a961...`):
`00000000036c6ce8  000018c500000402 R_AARCH64_JUMP_SLOT    0000000002726eac _ZN10NRadEngine31CSetCanvasElementAttributesNode5EnterERKNS_35SSetCanvasElementAttributesNodeDataE + 0`.
The function is libr15's own, so the target names it as the slot's expected original (`base + 0x2726eac`; the image is BIND_NOW, `got_hook.h`). The handler is a
`CallbackThunk` installed through `InstallThunk`; nothing is written to code.

The data layout is read from Enter's body, four instructions pinned in `hudmeter_targets.h` and checked against the real file by `got_pinned_test`:
`ldr w8, [x19, #0x34]` (0x272711c, the size flag: the size is written only when it is not 0), `ldr x2, [x19, #0x20]` (0x2727130, the item),
`add x3, x19, #0x38` (0x2727134, the size passed to `CCanvasUICS::SetElementSize`), and Enter's first instruction (0x2726eac). The canvas component at +0x18
is the reference build's reading; no word pins it and the module only logs it.

## What it fixes

The wrist HUD's weapon-slot heat fill (helmet script `500851025e2a0bee`, element `weapon_meter` = `9fd2e8cc0e46df3c`, canvas component
`e55af932c88ec864`) stays 0 x 0 on the Quest because of a start-order read-back trap: the first `SetCanvasElementAttributes(weapon_meter, size)` runs with a
zero-seeded state and writes 0 x 0; the next node reads weapon_meter's own size (now 0 x 0) back as the "full size" and keeps 0 for ever. The handler writes
the authored full size 86 x 88 for that one call.

## Scope: a combat level, and nothing else

Enter is a generic script node and arena's scripts call it too, on the same APK. The handler tests `scope::SeenCombatOrLobby()` first (`combat_scope.h`: a combat
map or the combat lobby `mpl_lobby_b_combat` was loaded at the last look and no Echo Arena level was, the look made four times a second by the game-thread tick). Outside a combat level it calls the
game's function with the same arguments and does nothing else: no read of the data, no write, no counter, no line. `hudmeter_test` pins that with data
pointers nobody could read, a 0 x 0 write that stays 0 x 0, zero counters, and by count zero reads of the game's memory and zero `level::ScanCalls()`, in each arena level, the lobbies arena players use, a combat-to-arena transition and
before the first look. The combat body is a separate `noinline` function, `ActInCombat`, that first asks `scope::LookIsFresh()`: when the tick has stopped for more than
a second, a true answer is not believed and the call passes through (pinned by `TheModuleFollowsTheLevelBothWays`). The first look after the tick is back writes one Warning, `combat_scope_stale` (`gap_ms`, `stale_after_ms`, `level`), because nothing could say so during the gap. The `scope` row registers the refresh only while a reader is on (`combat.hudmeter`, `combat.shieldgamma_gamma` or `combat.shieldgamma_tube_mirror`); with all three off the look is not made in any level.

The plugin copy this replaces read the first 0x40 bytes of the data with `process_vm_readv` on every call, in every level. In an arena match the module now costs
the call through the thunk and, in the handler, eight instructions with no frame, no stack canary and no call (NDK r26d clang -O2, `llvm-objdump -d`):
`adrp x8; mov x3, x0; ldr w8, [x8]; tbnz w8, #2; and w8, w8, #3; cbz w8; mov x0, x1; mov x1, x2; br x3`; in combat it costs what the plugin cost plus a clock read. How often Enter runs per frame is not measured (no headset run);
`hudmeter_test` prints the cost per call on the host (the game's function, through the hook in an arena level, and in a combat level for a call that is not the write).

## Mechanism

Per call, in a combat level:

1. One `SafeRead` of the first 0x40 bytes of the data argument (`process_vm_readv` on our own pid, which never faults). Unreadable: the game's function with the caller's own
   arguments, counted.
2. Not item `9fd2e8cc0e46df3c`, or size flag +0x34 == 0: the game's function untouched (no log).
3. Any canvas component is guarded; the first component other than `e55af932c88ec864` is named once.
4. A size of exactly 0 x 0 becomes 86 x 88 in the node-data buffer for that call, the game's Enter runs (it reads the size itself at data + 0x38), and the original bytes
   (0 x 0) are put back afterwards. The buffer is the calling script thread's own (`thread + 0xb0`); the range was just read, so it is mapped. Any other size: the game's
   function untouched.

## Lines (sentinel log)

`combat_hudmeter_install` (Info installed / Info off / Error refused, with the reason), `combat_hudmeter_full` for the 1st, 2nd, 4th, 8th ... guarded write, one
`combat_hudmeter_pass` per helmet start (the first other size after a run of full writes: the fill is live), `combat_hudmeter_fill` for the first 8 height changes only,
`combat_hudmeter_component` once. No per-call lines: `hudmeter_test` asserts 2000 calls log nothing. `combat_scope_change` says when a combat level was entered or left.

## Prediction (written before any headset run)

On a combat APK, in the lobby and then a match on the pinned build: `combat_hudmeter_install` `installed` at start; `combat_scope_change` `to: 1` when the combat lobby or
a combat map loads; when the helmet starts, one `combat_hudmeter_full` `n: 1` with `to_x: 86, to_y: 88`, then one `combat_hudmeter_pass` as soon as the first heat write arrives,
and the wrist weapon-slot bar fills with weapon heat. In an arena match on the same APK: `combat_scope_change` never says `to: 1`, no `combat_hudmeter_*` line other than the install
line, and no change anywhere in the arena HUD.

## Falsifiers

- `combat_hudmeter_install` `refused` with `OriginalMismatch`: the slot held another word (another hook got there first, or a different build). The module is inert.
- `combat_hudmeter_full` appears but no `pass` line ever follows and the bar stays empty: the script's later writes are also 0 x 0, or the full-size read-back happens through a
  path other than Enter.
- No `combat_hudmeter_full` line in a combat match although the bar is empty: `combat_scope_change` never said `to: 1`, so the level answer did not see the combat level (the
  tables or the level ids differ on this build), or the helmet starts before the first look (a quarter second after the level starts loading; the helmet script starts seconds later).
- A crash in `CSetCanvasElementAttributesNode::Enter` with the module on (a data buffer that is not writable or not the caller's own).

## Not built, not verified

- Not built: the reference build's rev 4 old-bar part and its `cvbind` (a log-only probe on the same slot: it changes no argument and no result).
- Not verified: that the Quest calls this node with item `9fd2e8cc0e46df3c` in a combat match (the reference build's first lobby log says yes), and the node's calls per frame.

# Module `hudmeter` (row R57)

Files: `src/hudmeter.{h,cpp}`, `src/quest/plugins/q15t/src/pbhud_log.h` (shared with powerboost / shieldgamma), `src/quest/tests/q15t/hudmeter_test.cpp`,
`src/quest/tests/q15t/pbhud_image.h`, `src/quest/tests/q15t/mod/hudmeter.sh` (+ `src/quest/tests/q15t/mod/pbhud_ndk.sh`), `src/quest/tests/q15t/mutants.d/hudmeter.sh`.

Option macro for the main seat to wire: `Q15T_HUDMETER`, default **ON (1)** (#1625: the wave19a on-list turns on `hudmeter.on`); `0` = off. The source:
`hudmeter.on` "Off: the switch is read, nothing else" (`<source build>/the source build's hudmeter:49`); the QA
recipe turns it on. API: `InstallResult InstallHudmeter(std::uintptr_t base)`; no tick.

## Row covered

R57: `NRadEngine::CSetCanvasElementAttributesNode::Enter(SSetCanvasElementAttributesNodeData const&)`, JUMP_SLOT ELF 0x36c6ce8,
real function ELF 0x2726eac = disassembly 0x2826eac (size 1600). (a source-build note, not in this tree):
`| R57 | D6 CSetCanvasElementAttributesNode::Enter -> 0x2826eac (1600) | node Enter | JUMP_SLOT 0x36c6ce8 | 1 | n/a | GOT |`.
Source constants `kGot = 0x36c6ce8, kFn = 0x2726eac` (the source build's hudmeter:76). disassembly entry words at 0x2826eac, read this
session: `fc6bbba9 f96301a9 f75b02a9 f55303a9` (`stp x28,x27,[sp,#-0x50]!` ...), equal to the first four of the source's 11 pinned
words (the source build's hudmeter:83: `{0x2726eac, 0xa9bb6bfc}, {0x2726eb0, 0xa90163f9}, ...`). All 11 source words match disassembly.
The `readelf` relocation `JUMP_SLOT 0x36c6ce8 -> 0x2726eac` is in `R0_readelf_r.txt`.

## What it fixes

The wrist HUD's weapon-slot heat fill (helmet script `500851025e2a0bee`, element `weapon_meter` = `9fd2e8cc0e46df3c`, canvas
component `e55af932c88ec864`) stays 0 x 0 on the Quest because of a start-order read-back trap (the source build's hudmeter:1-9): the
first `SetCanvasElementAttributes(weapon_meter, size)` runs with a zero-seeded G (+0xd4 / +0xd8) and writes 0 x 0; the next node reads
weapon_meter's own size (now 0 x 0) back as the "full size" and keeps 0 forever. The guard writes the authored full size 86 x 88
for that one call (`kFull`, `:79`, `:34-38`).

## Mechanism

`InstallImportHook(symbol, base, 0x36c6ce8, &Wrapper, &g_next, 0x2726eac)`: a data-page GOT swap; the slot must hold
`base + 0x2726eac` else the install is refused (`unexpected_original`, one ERROR line, slot untouched). The wrapper, per call:

1. One `SafeRead` of the first 0x40 bytes of the data argument (`SSetCanvasElementAttributesNodeData`: +0x18 component, +0x20 item,
   +0x34 size flag, +0x38 size x, +0x3c size y). Unreadable: the real function with the caller's own arguments, counted.
2. Not item `9fd2e8cc0e46df3c`, or size flag +0x34 == 0: the real function untouched (no log).
3. Any canvas component is guarded (source rev 3; the first component other than `e55af932c88ec864` is named once).
4. A size of exactly 0 x 0 becomes 86 x 88 in the node-data buffer for that call, the real Enter runs (it reads the size itself at
   data + 0x38), and the original bytes (0 x 0) are put back afterwards. The buffer is the calling script thread's own
   (`thread + 0xb0`, source comment the source build's hudmeter:47-48); the range was just read, so it is mapped. Any other size: the real
   function untouched.

Logging (Log, structured): INFO `hudmeter: on ...` at install (slot, fn, status, full size), ERROR on a refusal or
`restore_failed`; `hudmeter: full size written` for the 1st, 2nd, 4th, 8th ... guarded write (n, from, to, comp, item); one
`hudmeter: pass` per helmet start (the first other size after a run of full writes: the fill is live); `hudmeter: fill weapon_meter
size` for the first 8 height changes only; `another canvas component` once. No per-call lines (the node is called every script
tick): `src/quest/tests/q15t/hudmeter_test.cpp` asserts 2000 calls log nothing. Real records from the host test (`PBHUD_LOG=1`):
```
plugin=q15t hudmeter: on; a 0 x 0 size write to weapon_meter becomes the authored full size slot_elf_vaddr=0x36c6ce8 fn_elf_vaddr=0x2726eac status=ok reason= full_x=86 full_y=88
plugin=q15t hudmeter: full size written for a 0 x 0 weapon_meter write n=1 from_x=0 from_y=0 to_x=86 to_y=88 comp=0xe55af932c88ec864 item=0x9fd2e8cc0e46df3c
plugin=q15t hudmeter: pass; full size was read, the fill is live x=86 y=0 full_writes_before=1
```

## cvbind: does hudmeter need it? No.

the source build's cvbind (812 lines, "cvbind.on (TEST ONLY, log-only; RE-25 G31 ... DRAFT, not built, no AUD yet)", `:1`) swaps eleven JUMP_SLOTs, among
them this slot (`:83`: `0x36c6ce8, 0x2726eac`) and R58's (`:89`: `0x36d2020, 0x272d55c`), to find which nodes bind canvas / material
values. Its wrappers for these two slots only log and tail-call the stock function: `SceaWrap` (`:473-478`, ends
`return Stock<kScea>(node, data)`) and `SmpWrap` (`:570-578`, ends `return Stock<kSmp>(node, data)`). It changes no argument and no
result. hudmeter and shieldgamma only accept the slot holding stock or a wrapper from the source build's library and then call it as "the next hop"
(the source build's hudmeter:387-392, the source build's shieldgamma:213-217). So the guard's behaviour is independent of cvbind: nothing from cvbind
is built.

Chain order in the source (the source build's tap:174): `source-build cvbind_install()`, then `source-build hudmeter_install()`, then shieldgamma (the
source installs cvbind first; the later hook calls the earlier as next hop). In the plugin the slot has one owner: `InstallImportHook`
requires the slot to hold the stock word, so whichever module installs first wins and a second one is refused loudly. hudmeter owns
0x36c6ce8; shieldgamma owns 0x36d2020 (a different slot). A future port of cvbind would have to install after hudmeter and accept
the hudmeter wrapper as `expected`, or run before it and be chained to by it; none exists.

## Prediction (written before any headset run)

With `Q15T_HUDMETER=1`, in the lobby and then a match on the pinned build:
1. the log shows `hudmeter: on ... status=ok` at start;
2. when the helmet starts, one `hudmeter: full size written ... n=1 ... to_x=86 to_y=88`, then one `hudmeter: pass ... full_writes_before=1`
   as soon as the first heat write arrives; the wrist weapon-slot heat bar then fills with weapon heat instead of staying empty;
3. no more than 8 `hudmeter: fill` lines, no per-call lines; other canvas elements are unchanged.

## Falsifiers

- `hudmeter: OFF; nothing installed ... status=unexpected_original`: the slot held a different word (another hook got there
  first, or a different build). Module inert.
- `full size written` appears but no `pass` line ever follows and the bar stays empty: the script's later writes are also 0 x 0, or
  the full-size read-back happens through a path other than Enter (the guard is then the wrong place).
- A `full size written` line with `to_x` / `to_y` other than 86 / 88, or the bar at a wrong size: kFull is wrong for this build.
- The bar works in the lobby but not after a match load, with `another canvas component` never logged: the match uses a node
  that does not go through this slot.
- A crash in `CSetCanvasElementAttributesNode::Enter` with the module on (a data buffer that is not writable or not the caller's own).

## Needs a word patch / not built

- **Needs a word patch:** none (data-page GOT swap).
- Not built, by decision: the source's code-word pin check (`CodeMatches`, 11 words) and the md5 identity check: the plugin's check is
  the exact slot word (`base + 0x2726eac`) plus the contract's build id gate.
- Not built: the source's rev-4 "old bar" part (`oldbar.on` log, `oldbarfix.on` mirror, actor tables, the source build's hudmeter:11-32, `:132-282`).
  It is TEST-gated in the source (its own on-files), mirrors another element's writes across actors (a different behaviour with
  its own open audit items OBR-1..7), and the brief names only the weapon_meter fill. It rides the same slot, so it can be added to
  this wrapper later without a new row.
- Not built: cvbind (see above).
- Unverified: that the Quest actually calls this node with item `9fd2e8cc0e46df3c` (the source's first lobby log says yes);
  the data buffer offsets are from the source (`kDItem` 0x20, `kDSizeOn` 0x34, `kDSize` 0x38 agree with the source's reading of the
  function at ELF 0x272711c / 0x2727134, the source build's hudmeter:43-48; I have not re-decoded Enter's body).

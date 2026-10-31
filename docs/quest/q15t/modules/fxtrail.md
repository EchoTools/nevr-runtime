# Module `fxtrail` (row R29)

Files: `src/fxtrail.{h,cpp}`, `src/quest/tests/q15t/fxtrail_test.cpp`, `src/quest/tests/q15t/mod/fxtrail.sh`, `src/quest/tests/q15t/mutants.d/fxtrail.sh`.

Option macro for the main seat to wire: `Q15T_FXTRAIL`, default **ON** (the source build installs it unless switched off:
the source build's fxtrail:1 "ON by default", the source build's fxtrail:60-77 `Off()`; called unconditionally from the source build's tap:174).
API: `InstallResult InstallFxTrail(std::uintptr_t base)`; seam `InstallFxTrailAt(base, slot, real)`; `FxTrailEnded()`. No tick.
Runtime safety valve kept (the source's `fxtrail.off`): `<plugin data dir>/fxtrail.off` present at install, the module refuses
with reason `off_file` and one INFO line (`fxtrail off`); the main seat's `Report()` will show that as a non-ok status. The
source's other switch (`setprop debug.<app>.fxtrail 0`) is dropped (build option instead).

## Row covered

R29: `NRadEngine::CParticleEffectCS::EffectTime(SCompactPoolHandle) const`, JUMP_SLOT ELF 0x36e7b98, real function ELF 0x16647e4 =
disassembly 0x17647e4 (size 56). Read this session:
- `R0_readelf_r.txt:126983`: `00000000036e7b98  0000755300000402 R_AARCH64_JUMP_SLOT    00000000016647e4 _ZNK10NRadEngine17CParticleEffectCS10EffectTimeENS_18SCompactPoolHandleE + 0`
- Disassembly `functions`: `16647e4 | EffectTime | 56` (ELF = VA - 0x100000).
- `R1_functions.md:42`: `| R29 | B5 EffectTime -> 0x17647e4 (56) | returns particle table field +0x340 | JUMP_SLOT 0x36e7b98 | 1 | n/a | GOT |`.
- Disassembly instructions of the function (ELF address, word, text):
```
16647e4 086440f9 ldr x8, [x0, #0xc8]
16647e8 293c4092 and x9, x1, #0xffff
16647ec 29f57ed3 lsl x9, x9, #0x2
16647f0 0a038052 mov w10, #0x18
16647f4 08696978 ldrh w8, [x8, x9, LSL #0x0]
16647f8 097c40f9 ldr x9, [x0, #0xf8]
1664800 08250a9b madd x8, x8, x10, x9
1664804 080940f9 ldr x8, [x8, #0x10]
1664808 680000b4 cbz x8, 0x01764814
166480c 004143bd ldr s0, [x8, #0x340]
1664810 c0035fd6 ret
1664814 e003271e fmov s0, wzr
1664818 c0035fd6 ret
```
That is exactly the lookup the wrapper repeats (index table at cs+0xc8, u16 at byte 4*(h&0xffff); data array at cs+0xf8, stride 0x18;
instance pointer at +0x10), and the stock function answers 0.0 for a null instance. The source's constants: `kGotEffectTime =
0x36e7b98` (the source build's fxtrail:41), `kFnEffectTime = 0x16647e4`, `kCsIndex/kCsData/kDataStride/kDataInst = 0xc8/0xf8/0x18/0x10`
(`:42-43`), wrapper `:49-58`.

## What it does

Why (source the source build's fxtrail:1-24): a bullet is freed only once every FX slot reports done; a trail slot finishes only when
`EffectTime(h) > the CI's trail time` after impact. The Quest build creates no particle effect (source claim, the source build's fx, W-3;
not re-verified here), so the effect has no instance, `EffectTime` returns 0.0, no trail slot ever finishes, no bullet with a trail is
ever freed, and `CR15NetBulletCS::UpdateFX` queues one deferred decal-disable call per bullet per frame until the queue
overflows (SIGTRAP "CDeferredMethodQueue buffer has exceeded max number of calls (960)" ~53 s into combat).

Wrapper: call the real function first and keep its value `t`; read (SafeRead) `[cs+0xc8]`, `[cs+0xf8]`, the u16 at
`index + 4*(h & 0xffff)`, and the instance word at `data + idx*0x18 + 0x10`. Any read that fails, or a non-null instance: return `t`.
Null instance: return `FLT_MAX` and count it. The only caller in libr15 is `FXUpdateTrail+0x2b0` (source `:21-22`; `R1_functions.md`
counts 1 caller), so a data-page swap of the slot reaches every caller. Script libraries call it through their own GOTs and are
not touched (source `:22`).

Mechanism: `InstallImportHook(symbol, base, 0x36e7b98, &EffectTimeWrapper, &g_real, 0x16647e4)`: the slot must hold exactly
`base + 0x16647e4`, else the install is refused (ERROR, slot untouched). A data-page swap, no code word.

Logging (AGENTS.md): INFO `fxtrail on` (slot, real, status) at install; ERROR `fxtrail refused` with status/reason; INFO `fxtrail
off` for the kill file; the call path carries a counter only and logs `fxtrail null-effect trail ended ended=<n>` at n = 1, 2, 4, 8,
... (the source's cadence). Cost per call: the real call plus up to four `process_vm_readv` syscalls (the contract's "every read
through SafeRead"); the call rate of `EffectTime` is not measured (unverified).

## Gate: no MD5, no file hash

The only gates are: the plugin's build-id check in InitEx, the code-word pins below (`src/quest/plugins/q15t/src/word_pins.h`, `CheckWordPins`: every
word is read with SafeRead at `base + ELF vaddr` before any slot is touched) and the slot-target check (the slot must hold exactly
`base + the real function's vaddr`). **There is no MD5 gate and no whole-file hash of any kind**: a rebuilt, repacked or custom-server
build that still carries these words is not refused (the source build accepted three whole-file MD5s and refused every other
binary, which silently removed its crash guards). A pin that fails refuses the module (nothing swapped, reason `word_pin_mismatch`)
with one ERROR line naming it: `<module> refused: code word pin mismatch, ...  pin=0x<ELF vaddr> found=0x<word read, or unreadable>
expected=0x<word pinned>`. A slot-target refusal logs ERROR with `pin=<slot ELF vaddr> found=<word at the slot> expected=<base +
real vaddr>`. Pin addresses in this note are ELF vaddrs (disassembly VA = ELF + 0x100000). All 11 pins read in the disassembly
equal the table in `src/quest/plugins/q15t/src/fxtrail.h` (verified by a query that builds its VALUES from the header tables: `MATCH | 88` over the five
modules' tables, no NOT_SHOWN, no MISMATCH). Host test: every pin flipped in turn is refused and named, an unreadable text is refused and
named, the table's fingerprint is pinned, the installer refuses on a changed word (`src/quest/tests/q15t/pin_check.h`).

The source pins no code word for this row; the 11 pins here are an addition: the words of `EffectTime` the wrapper repeats the lookup
of (the 16647e4..1664810 listing above, 11 of its 13 instructions; the two after `ret` are not pinned).

## Differences from the source build

- The source's file/property switches become the build option plus the `fxtrail.off` valve (data dir
  `/sdcard/Android/data/<pkg>/files/q15t`, as weapon_arm). No md5 (see "Gate"); build id gated by InitEx, 11 code words and the slot word checked.
- Plain loads become SafeReads; failures return the real value (the source would have faulted).

## Prediction (written before any headset run)

On the pinned build, with the module on, in a combat match: the log shows `fxtrail on ... status=ok` at start, then
`fxtrail null-effect trail ended ended=1` within the first shots that hit something, later `ended=2`, `ended=4`, ... (powers of
two only). The ~53 s SIGTRAP "CDeferredMethodQueue buffer has exceeded max number of calls (960)" no longer occurs in a normal
leg. Nothing visible changes (there is no trail to see on Quest).

## Falsifiers

- `fxtrail refused: code word pin mismatch` (ERROR; pin / found / expected named) on the build the plugin accepted by build id: the code at that
  address is not what the wrapper was derived from (or the pin table has a typo); nothing is swapped, the game runs stock.
- The SIGTRAP at ~53 s still occurs with `ended>=1` logged: `EffectTime` is not the only thing keeping bullets alive.
- The SIGTRAP occurs and no `ended=` line was ever logged: the wrapper is not reached (the slot is not what FXUpdateTrail calls on
  this build) or the instance is non-null (the Quest build does create effects, so the source's premise is wrong).
- `fxtrail refused ... status=unexpected_original`: the slot word is not `base + 0x16647e4`.
- A visible change in trails or a crash inside `FXUpdateTrail`: the FLT_MAX answer is reaching a caller that does not expect it.

## Needs a word patch / not built

- **Needs a word patch:** none.
- Not built: the md5 gate (deliberately none); `setprop debug.<app>.fxtrail`.
- Unverified: the claim that the Quest build never creates a particle effect (source the source build's fx, W-3), and that `FXUpdateTrail` is
  the only caller (source test a source-build test, not re-run). I verified the slot, the function's name and size, and
  its 13 instructions above.

# Module `levelname` (row R04, the source build's "G7")

Files: `src/levelname.{h,cpp}`, `src/quest/tests/q15t/levelname_test.cpp`, `src/quest/tests/q15t/mod/levelname.sh`, `src/quest/tests/q15t/mutants.d/levelname.sh`.
Option macro: `Q15T_LEVELNAME`, default **ON** (the source combat build applies it); kill file `levelname.off` in the plugin data
directory turns levelname and boosterpool off together, and the bot-body gate with them (`src/quest/plugins/q15t/src/combat_gates.h`). It needs a
full-size global_mp level in the pack. API: `InstallResult InstallLevelName(std::uintptr_t base)`; no tick.

## Row covered

R04: `CR15Game::LoadLevelName` keeps the plain name for global_mp. Source build: `<source build>/the source build's patch table`
lines 370-409 (the G7 comment at 370, `LOWSPEC_SITE_PATTERN` 382, `GLOBAL_MP = 0x3F9915D3001DC28E` at 386,
`full_global_mp_patches` at 389). The source build rewrote the `tbnz` branch target into a code cave (code word write). This
module does the same decision in a wrapper reached through the vtable word instead (data page only, no code write).

## Stock function (ReVault binary 576, `instructions`, read this session)

```
1302ecc 085b8f52 mov   w8, #0x7ad8
1302ed0 086868b8 ldr   w8, [x0, x8, LSL #0x0]
1302ed4 4800f837 tbnz  w8, #0x1f, 0x01302edc
1302ed8 3e41f417 b     0x010133d0
1302edc e8cc00f0 adrp  x8, 0x2ca1000
1302ee0 08d92f91 add   x8, x8, #0xbf6              ("_lowspec")
1302ee4 22008052 mov   w2, #0x1
1302ee8 23008052 mov   w3, #0x1
1302eec e00301aa mov   x0, x1
1302ef0 e10308aa mov   x1, x8
1302ef4 ebbbf417 b     0x01031ea0
```
`functions`: `1302ecc|LoadLevelName|44`. Decompilation (raw): `void CR15Game::LoadLevelName(long param_1, undefined8 param_2)`:
`if (-1 < *(int *)(param_1 + 0x7ad8)) { CncaGame::LoadLevelName(); return; } CSymbol64::Append(param_2,"_lowspec",1,1);`.
So this = x0, name (CSymbol64, 64 bit, in x1); nothing else is read. The `_lowspec` string at ReVault 0x2ca1bf6 is
referenced from exactly two functions (`xrefs`): `LoadLevelName` (1302ef0) and `CR15NetBoosterCS::AllocateExclusive` (2210f54).

Callees, read the same way:
- `10133d0` (the plain tail call): `functions` says `thunk_FUN_018f162c | NRadEngine::CncaGame | 16`; bytes are a PLT stub:
  `adrp x16,0x37e1000; ldr x17,[x16,#0xd0]; add x16,x16,#0xd0; br x17`. Its GOT word is ELF 0x36e10d0, `readelf -rW`:
  `00000000036e10d0 R_AARCH64_JUMP_SLOT 00000000017f162c _ZNK10NRadEngine8CncaGame13LoadLevelNameENS_9CSymbol64E`.
  The target `18f162c` (size 8) is `mov x0, x1 ; ret` (`e00301aa c0035fd6`): returns the name, `undefined8 f(param_1,param_2)`.
  So the plain function is (this, name) -> name, both in registers, nothing on the stack; a PLT stub is safe to call as a
  function pointer (x16/x17 are the scratch registers it uses).
- `1031ea0` (Append) is likewise a PLT stub, JUMP_SLOT ELF 0x36f0638 -> `_ZN10NRadEngine9CSymbol646AppendES0_PKcjj`.
  Not called by the wrapper (the real function does that).

Register contract of the wrapper: `uint64_t Wrapper(void* game /*x0*/, uint64_t name /*x1*/)`, result in x0. Nothing in
x2.. is read by either callee (the stock function sets w2/w3 itself for Append), so none is forwarded.

## The six data xrefs (ReVault `xrefs`, to_address 1302ecc, binary 576), and what they are

ELF vaddr = ReVault VA - 0x100000; `readelf -rW` and `readelf -SW` of the pinned library: extracted from
the store APK (`lib/arm64-v8a/libr15.so`) into a new scratch directory (never run from it;
`readelf -n` there: `Build ID: b243509c08ce677aeb95fa348016949b3fc45230`, the pinned id).

| ReVault VA | ELF vaddr | section | what |
|---|---|---|---|
| 3350f34 | 3250f34 | .eh_frame_hdr (0x3249210..) | NOT a vtable word: 4 bytes `bc9cfbfd` = 0xfdfb9cbc = 0x1202ecc - 0x3249210 (the unwind search table entry) |
| 3428e10 | 3328e10 | .eh_frame (0x330ed50..) | NOT a vtable word: an FDE `pc_begin`, pc-relative 0xfdeda0bc: 0x3328e10 + (-34758468) = 0x1202ecc |
| 372ef90 | 362ef90 | .data.rel.ro | `R_AARCH64_ABS64 0000000001202ecc _ZNK10NRadEngine8NRadGame8CR15Game13LoadLevelNameENS_9CSymbol64E + 0`; in `_ZTVN10NRadEngine8NRadGame8CR15GameE` (0x362ed20, size 0x420) at +0x270 |
| 3730e58 | 3630e58 | .data.rel.ro | same reloc; `_ZTV...CR15NetDedicatedServerE` (0x3630be8, size 0x420) +0x270 |
| 3731278 | 3631278 | .data.rel.ro | same reloc; `_ZTV...CR15NetLanServerE` (0x3631008, size 0x420) +0x270 |
| 37316f8 | 36316f8 | .data.rel.ro | same reloc; `_ZTV...CR15NetServerE` (0x3631488, size 0x420) +0x270 |

Differences from the brief: the four relocations are `R_AARCH64_ABS64` against the exported symbol (value 0x1202ecc, addend
0), not `R_AARCH64_RELATIVE`. At run time the word is therefore the symbol's resolved address, which is base + 0x1202ecc
unless another loaded object defines the same name first (then the word differs and the install refuses). `.data.rel.ro` is
inside GNU_RELRO (0x35f9eb0 + 0x132150), so the pages are read-only at run time and `SwapWord` has to mprotect them
writable and back (exercised by the host test, "read-only vtable page").

Vtable slot: the symbol + 0x270; the vptr (address point) is symbol + 0x10, so the virtual offset is 0x260 (index 76).

## Which class dispatches, which vtables are swapped

Class tree (typeinfo base pointers, from `.data.rel.ro` relocations, checked with a script over all 3285 exported `_ZTV` symbols):
`CncaGame -> CRxGame -> CR15Game -> CR15NetServer -> {CR15NetLanServer, CR15NetDedicatedServer}`. Every class whose RTTI chain reaches
CR15Game has an exported vtable and each of these four holds CR15Game::LoadLevelName at +0x270; no class of the tree
overrides it. The two classes above CR15Game (CncaGame 0x3659ee0, CRxGame 0x36a2a88) hold `CncaGame::LoadLevelName`
(`17f162c`) in that slot and are not touched. Which of the four the running game object is on a Quest client is
**unverified**; all four are swapped, so it does not matter.

The caller (the only dispatch through slot 0x260 in a CncaGame/CR15Game/CncaGameSpace/CR15Net* function; the other `#0x260`
loads in those namespaces are `CR15NetGame::Broadcast<...>`, a different object): `CncaGameSpace::LoadLevel`
(`18e5484`):
```
18e5528 ldr x0, [x19, #0x8]      ; x19 = the gamespace, +8 = its CncaGame (the game)
18e552c mov x1, x22              ; x22 = the requested level name (CSymbol64)
18e5530 ldr x8, [x0]             ; vptr
18e5534 ldr x8, [x8, #0x260]     ; slot
18e5538 blr x8
18e553c ldrb w8, [x21, #0x1] ; 18e5540 mov x22, x0   ; the answer is the name the archive loader gets
```
The decompilation shows `uVar6 = (**(code **)(**(long **)(param_1 + 8) + 0x260))(*(long **)(param_1 + 8),param_2);` and uVar6 goes
to `CArchiveLoader::LoadArchiveAsync` / `CResourceInstance::LoadAndInit`. The function has no `bl` xref and no other
dispatch was found by the instruction scan (`ldr xN,[xM,#0x260]` followed within 3 instructions by `blr xN`: 3 non-PLT hits,
LoadLevel, `OutputTestsUI` with x1 = xzr and an unrelated generic wrapper `FUN_0297efe0` taking w0 back; neither is
this call). An indirect call from a place that scan cannot see is unverified.

## Mechanism and behaviour

`InstallLevelName(base)` swaps the four words (ELF vaddr 0x362ef90, 0x3630e58, 0x3631278, 0x36316f8) with `SwapWord(slot,
expected = base + 0x1202ecc, wrapper)`. A word holding anything else is refused. All four or none: if a word fails after
earlier ones were swapped, those are put back (the swap that failed wrote nothing) and one ERROR line names the slot, the
status and the roll-back count. A second install is refused ("already_installed").

Wrapper: `name == 0x3F9915D3001DC28E` and bit 31 of the 32-bit word at game+0x7ad8 (read with `SafeRead`; unreadable counts as
clear) -> call the plain function (PLT stub at base + 0xf133d0, ReVault 0x10133d0) with the same two arguments; anything else
-> call the real function (the value the words held, base + 0x1202ecc) with the same arguments. With the bit clear the real
function returns the plain name itself, so the wrapper only changes the answer in the one case the stock appends "_lowspec".

Logging (Log): INFO `levelname installed` (slots, real, plain, global_mp); ERROR `levelname refused` (slot_index, slot_vaddr,
why = SwapStatus name, found, expected, swapped_before, rolled_back); WARNING for a second install; INFO `levelname global_mp
kept its plain name` for the first 16 substitutions (name, n), then silent (counter `LevelNameKeptPlainCount()` keeps counting).
Ordinary levels log nothing. Real records from the host test run:
```
plugin=q15t levelname installed slots=4 real=0x7fad0d402ecc plain=0x7fad0d1133d0 global_mp=0x3f9915d3001dc28e
plugin=q15t levelname global_mp kept its plain name name=0x3f9915d3001dc28e n=1 lowspec=1 answer=plain name
plugin=q15t levelname refused slot_index=3 slot_vaddr=0x36316f8 why=unexpected_original found=0x7fad0d402ed0 expected=0x7fad0d402ecc swapped_before=3 rolled_back=3
```

## Companion row (not in this module): `booster_pool.plain_name`

**Provided as a GOT hook.** `src/quest/plugins/q15t/src/boosterpool.cpp` (the same `Q15T_LEVELNAME` option) wraps
`CncaGameSpace::AllocatePooledActor(CSymbol64)` through its GOT word (ELF 0x36cd1f8, the JUMP_SLOT of the PLT stub at ReVault
0xfeb620) and answers the plain name for the one suffixed name `AllocateExclusive` builds under the lowspec bit. The stock
`tbz` at ReVault 0x2210f40 is left alone. The word row, if the row table still applies it, is then a no-op for this path:
the call arrives with the plain name and passes through.

The word-row table's entry, as it states the byte patch:

Source comment (the source build's patch table): with the full global_mp, `CR15NetBoosterCS::AllocateExclusive` still looks for its
pooled actor named `<pool actor>_lowspec` when the lowspec bit is set; only the cut global_mp has that actor, so
`AllocatePooledActor` finds nothing and `CPhysicsCS::BodyTypeExclusionFlags` reads a null component: a crash 0.7 s into the
match ("the G6+G7 crash"). the word-rows table (not in this tree), row `booster_pool.plain_name`: `revault_va 0x2210dd4`, offset 364
= ReVault 0x2210f40, stock `0x36f8012a`, replacement `0x14000009`. Read in ReVault this session: `2210f40|2a01f836|tbz w10, #0x1f,
0x02210f64` inside `AllocateExclusive` (2210dd4, size 1616), preceded by `ldr w10,[x10,x11]`: the same pattern as the source's
`LOWSPEC_BOOSTER_PATTERN`. (The source comment says 0x2110f40; in this build it is 0x2210f40.) **Enable Q15T_LEVELNAME only
together with that row**, and with a full-size global_mp in the pack. This module cannot see whether the row is applied.

## Prediction (written before any headset run)

With `Q15T_LEVELNAME=1`, the pack holding the full global_mp (0x3f9915d3001dc28e) and `booster_pool.plain_name` applied:
1. the log shows `levelname installed slots=4` at start;
2. on the first global_mp load, one `levelname global_mp kept its plain name ... n=1`, and the archive loader is given
   0x3f99..., not 0x9a3717b576c23967; the match runs without the BodyTypeExclusionFlags crash;
3. every other level loads exactly as before (no extra log lines, no `levelname` line for them).

## Falsifiers

- `levelname refused ... why=unexpected_original found=<other>`: the ABS64 word resolved to a different address than
  base + 0x1202ecc (a preempting definition), or the base is wrong. The module is then inert (nothing left swapped).
- Installed, but a global_mp load produces no `kept its plain name` line while the level still loads as the cut one:
  the call did not reach the wrapper (an object of a class outside the four vtables, or the call is not through slot 0x260),
  or bit 31 of game+0x7ad8 was clear (then stock was already plain: not a failure of the module, check the flag).
- A crash 0.7 s into the match in `CPhysicsCS::BodyTypeExclusionFlags` with the line present: the booster row is missing.
- A load failure of 0x3f99... after the line: the pack has no full global_mp (the row's precondition).
- Any ordinary level changing its loaded name: the wrapper is wrong (host tests pin this).

## Needs a word patch / not built

- This module needs none. The companion `booster_pool.plain_name` is an existing code word row (stock `0x36f8012a` at
  ReVault 0x2210f40, replacement `0x14000009`), owned by the main seat's row table.
- Unverified: the dynamic class of the game object on the Quest; absence of other callers of the virtual slot outside the
  scanned function namespaces; the behaviour of the PLT stub at 0xf133d0 on the device (its bytes and GOT slot are read, the
  stub is not executed on the host, the test uses fakes through `SetLevelNameTargetsForTest`).
- Host-test limits: the fake image is a sparse anonymous mapping, the callees are fakes (the real/plain addresses are
  checked against base + the vaddrs, then swapped for fakes); the roll-back of a slot whose protection restore failed
  (`kRestoreFailed`) is not exercised.

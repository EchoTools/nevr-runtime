# Module `voipdrop` (row R30)

Files: `src/voipdrop.{h,cpp}`, `src/quest/tests/q15t/voipdrop_test.cpp`, `src/quest/tests/q15t/mod/voipdrop.sh`, `src/quest/tests/q15t/mutants.d/voipdrop.sh`.

Option macro for the main seat to wire: `Q15T_VOIPDROP`, default **ON** (the source build's voipdrop:1 "DEFAULT ON", `Switch()` `:114-130`;
installed from the source build's tap:174). API: `InstallResult InstallVoipDrop(std::uintptr_t base)` (resolves the four engine exports
itself); seam `InstallVoipDropAt(base, slot, real, fns)`; `VoipDropCount()`. No tick. Runtime safety valve kept (the source's
`voipdrop.off`): `<plugin data dir>/voipdrop.off` present at install: refused, reason `off_file`, one INFO line. The
`setprop` switch is dropped.

## Row covered

R30: `CR15NetVoipReceiverCS::ReceiveVoip(SCompactPoolHandle, SR15NetUserVoipEvent const&, void const*, unsigned)`, JUMP_SLOT ELF
0x36c5d80, real function ELF 0x21c3238 = disassembly 0x22c3238 (size 1192). Read this session:
- `R0_readelf_r.txt:109636`: `00000000036c5d80  00001da300000402 R_AARCH64_JUMP_SLOT    00000000021c3238 _ZN10NRadEngine8NRadGame21CR15NetVoipReceiverCS11ReceiveVoipENS_18SCompactPoolHandleERKNS0_20SR15NetUserVoipEventEPKvj + 0`
- Disassembly `functions`: `21c3238 | ReceiveVoip | 1192`; `R1_functions.md:43` (1 caller, the PLT stub; reached from UserVoipCB).
- Layout words (ELF address, word, text; all 10 of the source's ReceiveVoip/AllocPage words matched disassembly):
```
21c3238 d10203ff sub sp, sp, #0x80          ReceiveVoip entry
21c3698 9105a2a0 add x0, x21, #0x168        the voice hash table inside the receiver CS
23164d8 91006016 add x22, x0, #0x18         AllocPage: the table's allocator block (table+0x18)
2316504 b9406661 ldr w1, [x19, #0x64]       page bytes (table+0x64)
2316508 f9400908 ldr x8, [x8, #0x10]        allocator vtable +0x10 = the page request
23165a8 b9005269 str w9, [x19, #0x50]       the free head (table+0x50)
```
  and the whole AllocPage prologue 0x23164d0..0x231650c read in the disassembly: `bl <CMemBlock::Allocator>` (x21), `bl <CurrentPtr>` (x20, the
  saved context), `bl <SetCurrent>(x21)`, `bl <Current>`, `ldr x8,[x0]`, `ldr w1,[x19,#0x64]`, `ldr x8,[x8,#0x10]`, `blr x8` and the
  result is not tested before use. DestroyContext uses `ldr x8,[x8,#0x38]` at 0x124943c (vtable +0x38 = the free).
- Engine exports (disassembly names/sizes): `CMemBlock::Allocator` 0x11dc610 (8), `CMemoryContext::Current` 0x11c8dc4 (24), `CurrentPtr`
  0x11cae90 (12), `SetCurrent` 0x11cafac (20).
Source: constants the source build's voipdrop:42-43, probe `:82-94`, hook `:96-112`.

## What it does

Crash (source `:1-22`, tester dump c63511b8): `CHashTable<uint,ull>::AllocPage` asks the table's allocator for one page and never
checks the result; a NULL page is written from its end backwards (SIGSEGV at 0x7ffe0). The table frees every page each frame
(UpdateDefragment), so every packet after that needs a fresh page: a heap with no free 512 KiB block crashes on the next voice packet.

Wrapper: read the free head (`int32 [cs+0x1b8]`). If it is not -1, call the real function. If it is -1, read the page size
(`u32 [cs+0x1cc]`) and probe: save `CurrentPtr()`, `SetCurrent(Allocator(cs+0x180))`, `Current()`, call vtable `+0x10(a, bytes)`, give
a non-null block back through vtable `+0x38`, restore the saved context. A failed request (or no allocator at all) drops the
packet: the real function is not called, a counter is kept, one INFO line (first 16, then a running total every 50th drop, at most 16
totals). Doubt (self null/unreadable, page size unreadable, vtable unreadable or missing an entry) calls the real function.
The probe is not a reservation: it narrows the crash to a race, it does not close it (source `:17-20`).

Mechanism: `InstallImportHook(symbol, base, 0x36c5d80, &RecvWrapper, &g_real, 0x21c3238)`: the slot must hold exactly `base + 0x21c3238`
or the install is refused. The four engine functions are resolved with `dlsym` on the loaded game library (all four must exist),
else the install is refused with reason `export_missing` and an ERROR line. Data-page swap only.

Logging: INFO `voipdrop on` (slot, real, status), ERROR `voipdrop refused`, INFO `voipdrop off` (kill file), INFO `voipdrop no memory for a
voice table page: packet dropped page_bytes=<n> dropped=<n>` (capped as above). No per-packet line.

## Gate: no MD5, no file hash

The only gates are: the plugin's build-id check in InitEx, the code-word pins below (`src/quest/plugins/q15t/src/word_pins.h`, `CheckWordPins`: every
word is read with SafeRead at `base + ELF vaddr` before any slot is touched) and the slot-target check (the slot must hold exactly
`base + the real function's vaddr`). **There is no MD5 gate and no whole-file hash of any kind**: a rebuilt, repacked or custom-server
build that still carries these words is not refused (the source build accepted three whole-file MD5s and refused every other
binary, which silently removed its crash guards). A pin that fails refuses the module (nothing swapped, reason `word_pin_mismatch`)
with one ERROR line naming it: `<module> refused: code word pin mismatch, ...  pin=0x<ELF vaddr> found=0x<word read, or unreadable>
expected=0x<word pinned>`. A slot-target refusal logs ERROR with `pin=<slot ELF vaddr> found=<word at the slot> expected=<base +
real vaddr>`. Pin addresses in this note are ELF vaddrs (disassembly VA = ELF + 0x100000). All 10 pins read in the disassembly
equal the table in `src/quest/plugins/q15t/src/voipdrop.h` (verified by a query that builds its VALUES from the header tables: `MATCH | 88` over the five
modules' tables, no NOT_SHOWN, no MISMATCH). Host test: every pin flipped in turn is refused and named, an unreadable text is refused and
named, the table's fingerprint is pinned, the installer refuses on a changed word (`src/quest/tests/q15t/pin_check.h`).

The 10 pins are the source's `kWords` (the source build's voipdrop:55-64): ReceiveVoip entry, the table address / FindOrCreate call, the PLT
stub, AllocPage's block / page-size / allocator loads and the free-head store.

## Differences from the source build

- md5 (3 builds) is deliberately not carried (see "Gate"); the 10 code-word pins are kept; the file/property switches are replaced by the
  option and the `voipdrop.off` valve in the plugin data dir.
- Plain loads become SafeReads (a bad vtable is "doubt: real runs", not a fault); `dlclose` of the NOLOAD handle after each resolve.

## Prediction (written before any headset run)

In normal play the module is invisible: `voipdrop on ... status=ok` at start, and **no** `voipdrop no memory` line in a session
with free heap. Under heap exhaustion (the crash's precondition) the log shows `voipdrop no memory for a voice table page: packet
dropped page_bytes=524288 dropped=1` instead of the SIGSEGV in `AllocPage+0xc0` (`FindOrCreate+0xa4` <- `ReceiveVoip+0x480`); voice
from that peer is dropped for those packets and resumes when memory returns. `page_bytes` should read 524288 (0x4000 x 32).

## Falsifiers

- `voipdrop refused: code word pin mismatch` (ERROR; pin / found / expected named) on the build the plugin accepted by build id: the code at that
  address is not what the wrapper was derived from (or the pin table has a typo); nothing is swapped, the game runs stock.
- A `voipdrop no memory` line in a normal session on a roomy heap: the layout offsets (+0x1b8, +0x1cc, +0x180) or the vtable slots are
  wrong, the probe fails for a reason other than memory, and voice is being dropped without cause. (Remote players inaudible.)
- `page_bytes` other than 524288 in a drop line: the page-size field is not what AllocPage uses.
- The same SIGSEGV (AllocPage <- FindOrCreate <- ReceiveVoip) with no preceding drop line: the packet came through with the probe
  succeeding and the real request failing (the declared race), or the head test never saw -1.
- `voipdrop refused ... export_missing`: one of the four memory exports is not in this library.

## Needs a word patch / not built

- **Needs a word patch:** none.
- Not built: the md5 gate (deliberately none), `setprop` switch of the source (see above); UpdateDefragment's own re-insert is not covered
  (source `:19-20`, also not covered there).
- Unverified: the allocator's vtable layout beyond the two slots read in the disassembly (+0x10 alloc at AllocPage 0x2316508, +0x38 at
  DestroyContext 0x124943c, the second being the source's statement that it is the free "DestroyContext uses"); the exact
  signature of the four memory exports (taken from the source).

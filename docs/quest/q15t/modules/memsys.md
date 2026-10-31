# Module `memsys` (rows R37 R38): render-target layout pool 512 -> 1024, and the android_main timing witness

Files: `src/memsys.{h,cpp}`, `src/quest/plugins/q15t/src/module_switch.h` (shared with `heaps`), `src/quest/tests/q15t/memsys_test.cpp`, `src/quest/tests/q15t/mod/memsys.sh`,
`src/quest/tests/q15t/mutants.d/memsys.sh`. Option macro for the main seat to wire: `Q15T_MEMSYS`, default **ON** (the source build arms memsys on every launch,
the source build's memsys:214-226). API: `InstallResult InstallMemsys(std::uintptr_t base)`; no tick. Per-module switch: `memsys.off`
(in `/sdcard/Android/data/<pkg>/files/q15t/`): installs nothing and also makes `heaps` leave R07 stock.

Source build: `<source build>/the source build's memsys` (M2 at lines 11-17, 65-77, 96-105; M1 is the `heaps` module).
Tester crashes it answers: F (System arena dry) and E (the 512-slot layout pool), the source build's memsys:4.

## Rows

| Row | Function | Slot (ELF) | readelf line | Real function (ELF / disassembly) |
|---|---|---|---|---|
| R37 | `CSimplePool<SGRenderTargetLayout,uint>::Alloc` | JUMP_SLOT 0x36e9b48 | 127997 | 0x1901b5c / `1a01b5c` |
| R38 | `CSysJava::Init(android_app*)` | JUMP_SLOT 0x36de5c0 | 122188 | 0xf7e8e8 / `107e8e8` |

Verbatim: `00000000036e9b48  00002a3d00000402 R_AARCH64_JUMP_SLOT    0000000001901b5c _ZN10NRadEngine11CSimplePoolINS_20SGRenderTargetLayoutEjE5AllocEjPNS_10CAllocatorE + 0`
and `00000000036de5c0  0000e13400000402 R_AARCH64_JUMP_SLOT    0000000000f7e8e8 _ZN10NRadEngine8CSysJava4InitEP11android_app + 0`
(a source-build note, not in this tree). Both functions are in libr15 itself, so the hook passes `expected_vaddr` to
`InstallImportHook` and the slot must hold `base + that` (BIND_NOW image, as the repack's got_hook.h requires).

## What R37 does (port of the layout-pool part of the source build's memsys)

`CGRawInterface::InitCommon` makes the pool with `Alloc(pool, 0x200, allocator)` (disassembly `19818b8 mov w1,#0x200`; `19818c8 bl 0x010248c0`,
the PLT stub whose `adrp x16,0x37e9000; ldr x17,[x16,#0xb48]` reads this very slot; return address `19818cc`). `Alloc` sizes the slots,
the free stack (`+0x28`, capacity in `+0x38`, `str x21,[x19,#0x38]` at `1a01c30`; `str x0,[x19,#0x28]` at `1a01c4c`), the capacity (`+0x84`)
and leaves the free count (`+0x30`) 0 (`1a01c24 stp q0,q0,[x22]`, x22 = pool + 0x20). InitCommon then pushes indices 511..0 at
`stack[count..]` inline and stores `count + 512` (`19818e8 add x9,x11,#0x200`; `1981904 cmp x8,#0x800`; `1981914 str x9,[x19,#0x30]`).

The wrapper, only when the return address is `base + 0x18818cc` and the capacity argument is 0x200, calls the real `Alloc` with
0x400. Then it validates the layout it just got (stack pointer non-null and 4-aligned, stack capacity >= 0x400, capacity word ==
0x400, count == 0, and the first and last free-stack entries readable by `SafeRead`), writes 1023..512 to `stack[0..511]`, and sets
count = 512. The inline fill then writes 511..0 at `stack[512..1023]` and stores count 1024: all of 0..1023 exactly once. The result
`Alloc` returned goes back to the caller untouched. Any other caller, any other capacity, or any layout it does not recognise
goes to the real function with the stock arguments: the pool is then the stock 512, with one WARNING line (once):
`memsys: layout pool NOT raised (layout not recognised)` with `stack, stack_cap, cap_read, count, readable`. On success one INFO line:
`memsys: layout pool raised` with `slots=1024 pre_pushed=512 cap_read_back=1024`.

The source build's claim that the layout index is used as a full 32-bit value (an audit item of the source build: no 512-bound consumer) is
the source build's memsys:17; I did not re-verify it (unverified here).

Differences from the source build: no md5 wait (the plugin loader already gated on the build id), no thread, and R38 is only the
witness, not the arm. The source's M1 (System heap) is the `heaps` module R07.

Timing of R37: the pool is made in InitCommon, after the game's init; whether that is after the plugin's init in the normal phase
was not traced (unverified). The wrapper is installed in whichever phase the plugin loads; if the pool was already made, the
effect is absent and nothing breaks (the witness and counters tell). With `early: true` (needed for `heaps`) it is in before.

## What R38 does: a timing witness, nothing else

android_main's first call is CSysJava::Init (disassembly `10784e4 bl 0x0100ddb0`; stub `100ddb0 adrp x16,0x37de000`, `100ddb4 ldr x17,[x16,#0x5c0]`;
return address `10784e8`). That precedes every heap-size read in android_main (`1078524`, `1078590`, `1078614`), so a wrapper on this slot that
is called from the site is proof the plugin was installed before android_main reached the reads. The first such call logs once:

`memsys: android_main reached CSysJava::Init` with `system_heap_mib_next_read` (the u64 the System GLOB_DAT word points at right now:
640 when `heaps` swapped it, 400 when it did not), `system_slot` (the GLOB_DAT slot address), `plugin_armed_before_android_main=1`.
The wrapper then calls the real function with the same argument and returns its result. **No line = the plugin loaded after
android_main passed Init**: `heaps` R06-R08 then cannot have taken effect (see `docs/quest/q15t/modules/heaps.md`, section 1).
(`plugin_armed_before_android_main` is a constant 1 by construction: the line can only be written by a call that came through the
plugin's own wrapper, installed beforehand.)

## Pinned code words (all 21 re-read from the disassembly)

Each word the wrapper's reasoning relies on is checked at install (`WordsMatch`); a mismatch refuses that row only, with a WARNING
naming the vaddr, the word found and the word wanted, and an ERROR `memsys hook install ... status=unexpected_original`. Source:
the source build's memsys:65-77. Verbatim from the disassembly (`instructions`, little-endian bytes compared with the table, all `t`):
`100ddb0 adrp x16,0x37de000`, `100ddb4 ldr x17,[x16,#0x5c0]`, `10248c0 adrp x16,0x37e9000`, `10248c4 ldr x17,[x16,#0xb48]`,
`10784e4 bl 0x0100ddb0`, `10784e8 bl 0x00fe5b00`, `1078510 adrp x8,0x381d000`, `1078514 ldr x8,[x8,#0x20]`, `1078524 ldr x1,[x8]`,
`19818b8 mov w1,#0x200`, `19818c8 bl 0x010248c0`, `19818cc ldp x10,x11,[x19,#0x28]`, `19818d8 mov w12,#0x1fb`,
`19818e0 movi v0.4S,#0x1,MSL #8`, `19818e8 add x9,x11,#0x200`, `1981904 cmp x8,#0x800`, `1981914 str x9,[x19,#0x30]`,
`1a01b5c sub sp,sp,#0x50`, `1a01c24 stp q0,q0,[x22]`, `1a01c30 str x21,[x19,#0x38]`, `1a01c4c str x0,[x19,#0x28]`
(21 rows, `match = t` for every one). The ELF vaddr in `src/quest/plugins/q15t/src/memsys.cpp` is the disassembly VA minus 0x100000.

## Logging (AGENTS.md)

`memsys hook install` per row (INFO ok, ERROR refusal) with `row, symbol, status, reason`; the one-time pool lines above; the one-time
witness line; `memsys off` (INFO) for the switch; `memsys not installed: no game base` (ERROR). The per-call path logs nothing (the
Alloc wrapper is called for every pool; counters only: `GetMemsysStats()`: alloc_calls, pool_raised, pool_not_raised, init_calls,
init_witnessed). No secret is logged. The host test asserts the lines.

## Prediction (written before any headset run) and falsifiers

Prediction: pinned build, plugin loaded `early: true`: logcat shows `memsys hook install` twice with `status=ok`, then
`memsys: android_main reached CSysJava::Init ... system_heap_mib_next_read=640`, then, when the render interface is initialised,
`memsys: layout pool raised slots=1024 pre_pushed=512 cap_read_back=1024`; the E-class crash (layout pool exhausted at 512) does not occur.

Falsifiers:
- `status=unexpected_original` on either row: the slot is not what readelf says (another hook, or a different build).
- `layout pool NOT raised` with `count != 0` or `cap_read != 1024`: the layout is not what Alloc's body says; nothing was written.
- The witness line never appears: the plugin loaded after android_main's first call (normal phase). Then R06-R08 did not take effect.
- `layout pool raised` followed by a crash in a layout consumer: the "no 512-bound consumer" claim (AUD-7) is wrong for this build.
- No `layout pool raised` line at all although the render interface is up: the pool was made before the wrapper was installed.

## Host tests and mutants

`Q15T_MODULE=memsys bash src/quest/tests/q15t/run_host_tests.sh` -> `memsys_test: all checks passed`. The fake library is one anonymous mapping holding
the pinned words and the two read-only slots; the fake `Alloc` builds the real layout; a fake inline fill pushes 511..0; the test
asserts the free stack is 0..1023 exactly once, the pass-through for the wrong caller and wrong capacity, five unrecognised
layouts (count not 0, no stack, capacity read back wrong, stack capacity short, stack unreadable), the witness once, word and slot
refusals per row, `memsys.off`, and base 0. 30 mutants in `src/quest/tests/q15t/mutants.d/memsys.sh`, one per rule; each is killed (numbers in the
report). The "no stack pointer" rule has no mutant of its own: its check is also covered by the readability check (equivalent).
A test cannot make a call return into libr15: the return-address gate is exercised through two test functions whose return addresses the
test passes in through `SetMemsysSitesForTest`; that the real site's address is `base + 0x18818cc` is the pin test and the disassembly read above.

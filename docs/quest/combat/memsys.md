# Module `memsys` (rows R37 R38): render-target layout pool 512 -> 1024, and the android_main timing witness

Files: `src/quest/combat/memsys.{h,cpp}`, `src/quest/combat/memsys_targets.h` (the pins), `src/quest/tests/memsys_test.cpp`,
`src/quest/tests/heap_step_test.cpp`, `src/quest/tests/got_pinned_test.cpp`. The module is the sentinel's early combat step, registered after `heaps`
(`src/quest/combat/combat_install.cpp`): it runs in the `q15t` and `combat` APKs and in no other. Switch: `combat.memsys` (default true) in
`config.yaml`; false installs nothing here and also leaves the System heap (`docs/quest/combat/heaps.md`, R07) as the game has it.

Origins: the layout-pool raise and the 21 pinned words are afk_memsys.cpp:11-17,65-77,96-105 of the reference build's source
(pinned in `docs/audits/2026-11-05-q15t-origin-citations.md`); the audit item that the layout index is used as a full 32-bit value is afk_memsys.cpp:17 of
the same record and was not re-verified here. The tester crashes it answers are the System arena running dry and the 512-slot layout pool running out.

## Rows

| Row | Function | Slot (ELF) | readelf line | Real function (ELF / disassembly) |
|---|---|---|---|---|
| R37 | `CSimplePool<SGRenderTargetLayout,uint>::Alloc` | JUMP_SLOT 0x36e9b48 | 127997 | 0x1901b5c / `1a01b5c` |
| R38 | `CSysJava::Init(android_app*)` | JUMP_SLOT 0x36de5c0 | 122188 | 0xf7e8e8 / `107e8e8` |

Verbatim: `00000000036e9b48  00002a3d00000402 R_AARCH64_JUMP_SLOT    0000000001901b5c _ZN10NRadEngine11CSimplePoolINS_20SGRenderTargetLayoutEjE5AllocEjPNS_10CAllocatorE + 0`
and `00000000036de5c0  0000e13400000402 R_AARCH64_JUMP_SLOT    0000000000f7e8e8 _ZN10NRadEngine8CSysJava4InitEP11android_app + 0`.
Both functions are in libr15 itself, so each target names the function as the slot's expected original (`base + that`; the image is BIND_NOW, as
`src/quest/sentinel/got_hook.h` requires). Each wrapper is a `CallbackThunk` with `kCaller`, installed through `InstallThunk`; the handler is told the
return address of the game's call and compares it with `base + 0x18818cc` (R37) or `base + 0xf784e8` (R38).

## What R37 does (port of the layout-pool part of the source build's memsys)

`CGRawInterface::InitCommon` makes the pool with `Alloc(pool, 0x200, allocator)` (disassembly `19818b8 mov w1,#0x200`; `19818c8 bl 0x010248c0`,
the PLT stub whose `adrp x16,0x37e9000; ldr x17,[x16,#0xb48]` reads this very slot; return address `19818cc`). `Alloc` sizes the slots,
the free stack (`+0x28`, capacity in `+0x38`, `str x21,[x19,#0x38]` at `1a01c30`; `str x0,[x19,#0x28]` at `1a01c4c`), the capacity (`+0x84`)
and leaves the free count (`+0x30`) 0 (`1a01c24 stp q0,q0,[x22]`, x22 = pool + 0x20). InitCommon then pushes indices 511..0 at
`stack[count..]` inline and stores `count + 512` (`19818e8 add x9,x11,#0x200`; `1981904 cmp x8,#0x800`; `1981914 str x9,[x19,#0x30]`).

The wrapper, only when the return address is `base + 0x18818cc` and the capacity argument is 0x200, calls the real `Alloc` with
0x400. Then it validates the layout it just got (stack pointer non-null and 4-aligned, stack capacity >= 0x400, capacity word ==
0x400, count == 0, and the whole free stack readable and writable in `/proc/self/maps`), writes 1023..512 to `stack[0..511]`, and sets
count = 512. The inline fill then writes 511..0 at `stack[512..1023]` and stores count 1024: all of 0..1023 exactly once. The result
`Alloc` returned goes back to the caller untouched. Any other caller, any other capacity, or any layout it does not recognise
goes to the real function with the stock arguments: the pool is then the stock 512, with one WARNING line (once):
`memsys: layout pool NOT raised (layout not recognised)` with `stack, stack_cap, cap_read, count, readable`. On success one INFO line:
`memsys: layout pool raised` with `slots=1024 pre_pushed=512 cap_read_back=1024`.

The source build's claim that the layout index is used as a full 32-bit value (an audit item of the source build: no 512-bound consumer) is
the source build's memsys:17; I did not re-verify it (unverified here).

Differences from the reference build: no md5 wait (the build ID is checked before the step), no thread, and R38 is only the
witness, not the arm. The reference's System-heap change is the `heaps` module's R07.

Timing of R37: the pool is made in InitCommon, after `android_main` starts; the step runs in the sentinel's constructor, before it (`docs/quest/combat/heaps.md`,
section 1), so the wrapper is in before the pool is made. The pool line (`combat_memsys_pool`) is written after `Alloc` returned.

## What R38 does: a timing witness, nothing else

android_main's first call is CSysJava::Init (disassembly `10784e4 bl 0x0100ddb0`; stub `100ddb0 adrp x16,0x37de000`, `100ddb4 ldr x17,[x16,#0x5c0]`;
return address `10784e8`). That precedes every heap-size read in android_main (`1078524`, `1078590`, `1078614`), so a wrapper on this slot that
is called from the site is proof the step installed before android_main reached the reads. The first such call logs once:

`combat_memsys_witness` with `system_heap_mib_next_read` (the u64 the System GLOB_DAT word points at right now:
640 when `heaps` swapped it, 400 when it did not), `system_heap` (`raised`, `stock` or `unreadable`) and `system_slot_vaddr`.
The wrapper then calls the real function with the same argument and returns its result. **No line = the step ran after
android_main passed Init**: `heaps` R06-R08 then cannot have taken effect (see `docs/quest/combat/heaps.md`, section 1). The line can only be written by a
call that came through this wrapper, installed beforehand.

## Pinned code words (all 21 re-read from the disassembly)

Each word the wrapper's reasoning relies on is checked at install (`WordsMatch`); a mismatch refuses that row only, with a WARNING
naming the vaddr, the word found and the word wanted, and an ERROR `memsys hook install ... status=unexpected_original`. Source:
the source build's memsys:65-77. Verbatim from the disassembly (`instructions`, little-endian bytes compared with the table, all `t`):
`100ddb0 adrp x16,0x37de000`, `100ddb4 ldr x17,[x16,#0x5c0]`, `10248c0 adrp x16,0x37e9000`, `10248c4 ldr x17,[x16,#0xb48]`,
`10784e4 bl 0x0100ddb0`, `10784e8 bl 0x00fe5b00`, `1078510 adrp x8,0x381d000`, `1078514 ldr x8,[x8,#0x20]`, `1078524 ldr x1,[x8]`,
`19818b8 mov w1,#0x200`, `19818c8 bl 0x010248c0`, `19818cc ldp x10,x11,[x19,#0x28]`, `19818d8 mov w12,#0x1fb`,
`19818e0 movi v0.4S,#0x1,MSL #8`, `19818e8 add x9,x11,#0x200`, `1981904 cmp x8,#0x800`, `1981914 str x9,[x19,#0x30]`,
`1a01b5c sub sp,sp,#0x50`, `1a01c24 stp q0,q0,[x22]`, `1a01c30 str x21,[x19,#0x38]`, `1a01c4c str x0,[x19,#0x28]`
(21 rows, `match = t` for every one). The ELF vaddr in `src/quest/combat/memsys_targets.h` is the disassembly VA minus 0x100000.

## Logging (AGENTS.md)

`combat_memsys_hook` per row (Info installed, Error refused, Warning for a pinned word that differs, with the word's address) with `row`, `symbol`,
`status`; `combat_memsys_install` once (`result` `installed|partial|refused|off`, `reason`, `r37`, `r38`, the consequence); `combat_memsys_pool` once, written after
`Alloc` returned (Info `raised`, Warning `not_raised` with the words read: `stack`, `stack_cap`, `cap_read`, `count`, `readable`); `combat_memsys_witness`
once. The per-call path logs nothing; the wrapper keeps counters (`memsys::CurrentStats()`). The sentinel's `combat_install_module` record for `memsys` (phase `early`)
carries its `outcome` and `status`. No secret is logged. `src/quest/tests/memsys_test.cpp` asserts the lines.

## Prediction (written before any headset run) and falsifiers

Prediction: pinned build, a combat APK: the sentinel log shows `combat_memsys_hook` twice with `status=ok`, then
`combat_memsys_witness ... system_heap_mib_next_read=640 system_heap=raised`, then, when the render interface is initialised,
`combat_memsys_pool result=raised slots=1024 pre_pushed=512 cap_read_back=1024`; the E-class crash (layout pool exhausted at 512) does not occur.

Falsifiers:
- `status=original_mismatch` on either row: the slot is not what readelf says (another hook, or a different build).
- `combat_memsys_pool result=not_raised` with `count != 0` or `cap_read != 1024`: the layout is not what Alloc's body says; nothing was written.
- The witness line never appears: the step ran after android_main's first call. Then R06-R08 did not take effect.
- `result=raised` followed by a crash in a layout consumer: the "no 512-bound consumer" claim is wrong for this build.
- No `combat_memsys_pool` line at all although the render interface is up: the pool was made before the wrapper was installed.

## Host tests and mutants

`just test-quest-combat memsys` runs `memsys_test` (the pool is 0..1023 exactly once after the inline fill; the wrong caller and the wrong capacity pass through; five layouts the module does not recognise and a stack in read-only memory leave the stock 512 with one Warning; the witness once, raised or stock; a pinned word that differs and a slot another hook owns each refuse their row alone; the switch; the missing and the wrong image; once; the golden lines) and `heap_step_test`. A test cannot make a call return into libr15: the return-address gate is exercised through two test functions whose return addresses the test passes in through `SetSitesForTest`; that the real sites are `base + 0x18818cc` and `base + 0xf784e8` is the pin table, which `just test-quest-hooks-pinned` reads in the real libr15 (`got_pinned_test`).

`src/quest/combat/mutants.sh memsys` runs `mutants.d/memsys.sh`: each rule of `memsys.cpp` and `memsys_targets.h` (every pin and each of the 21 code words, each doubt that refuses to
write the pool, the return-address gates, the witness's classes, the install line for each outcome) removed or bent once, and `memsys_test` (or `heap_step_test` for the rows only the
shipped table sees) must fail on each; the script's header names the mutants no run can tell from the original and why. The `msrow_` mutants edit the shared install table and are run
apart (`mutants.sh memsys '^msrow_'`).

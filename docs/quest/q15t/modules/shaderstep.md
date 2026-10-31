# Module `shaderstep` (issue #1553): CGShaderSetResource table growth step 32 -> 1

Files: `src/quest/plugins/q15t/src/shaderstep.{h,cpp}`, `src/quest/tests/q15t/shaderstep_test.cpp`, `src/quest/tests/q15t/mod/shaderstep.sh`.
Option: `Q15T_SHADERSTEP` (default ON). Off switch: `shaderstep.off` in the q15t data directory.

## What the game does (pinned build b243509c, libr15, the disassembly of `libr15.so`)

- `CFactory<CGShaderSetResource>::ConstructInstance`, disassembly 0x1a06f4c, size 304 (ELF 0x1906f4c). It builds the object in place
  and, on its common tail, stores the table growth step: `0x1a07000 mov w8,#0x20` then `0x1a07004 str x8,[x19,#0x98]`
  (from the disassembly, the tail is shared by both branches of the `cbz x2`).
- The table is the CTableXT at object+0x78: CMemBlock first, step at table+0x20 (= object+0x98), capacity at +0x28. The layout
  is the one custctx reads for `CTableXT<SR15NetCustomDrawData>` (`src/quest/plugins/q15t/src/custctx.cpp`, `DrawTable`).
- The factory's vtable: the destructor `~CFactory` (disassembly 0x1a06ef0) stores `_ZTV+0x10` into the object. Its GOT load at
  0x1a06ef8 (`ldr x8,[x8,#0xcb8]`) then `add x8,x8,#0x10` gives the address point disassembly 0x375ddc0. Entries there (xrefs to each
  function VA): 0x375ddc0 -> 0x1a06ef0 (slot 0, `~CFactory`), 0x375ddc8 -> 0x1a06f4c (slot 1, `ConstructInstance`),
  0x375ddd0 -> 0x1a0707c (slot 2, `CreateInstance`), 0x375ddd8 -> 0x1a071a0 (slot 3, `CreateData`),
  0x375dde0 -> 0x1a071a8 (slot 4, `SizeOfInstance`). ELF = disassembly VA - 0x100000, so the wrapped slot is ELF 0x365ddc8.
- `CreateInstance` (disassembly 0x1a0707c) also stores 0x20 at its own `puVar3[0x13]` (disassembly 0x1a07130, `mov w8,#0x20`). It is
  NOT changed here (the issue names the constructor). See "Not covered".

## Mechanism

- `InstallImportHook` (`import_hook.cpp`) with `expected_vaddr` 0x1906f4c: the slot must hold `base + 0x1906f4c` (the
  relocated value of a RELA slot that the file stores as zero, as the vtable's zero words at disassembly 0x375ddb8..0x375ddc8 show).
  A slot that holds anything else is refused with `unexpected_original` and nothing is written.
- The wrapper calls the game's own `ConstructInstance` through the saved address, then `ApplyShaderSetStep` (`shaderstep.cpp`):
  it reads the word at object+0x98, writes 1 only if it is the game's 0x20, and reads back. Any other word is left alone (WARNING,
  counted). No code page is written; the vtable page is a data page (`got_swap.h` refuses executable pages).

## Not shown (unverified, and why it matters)

- That the runtime's vtable is the one at 0x375ddc0. The destructor shows the vtable pointer it stores; no run has confirmed it.
- That the slot's runtime value is `base + 0x1906f4c`. If the image uses pointer authentication for vtable entries, the word
  differs and the install is refused (`unexpected_original`), which is the loud failure.
- Disassembly also shows two other tables with the same entries: 0x3387f8c/0x3387f94 (the same functions in an 8-byte stride) and
  0x34fb0c8/0x34fb0e0. Their words do not read as plain pointers in the file. Whether a factory object is ever built through them
  is not shown; this module swaps only the 0x365ddc8 slot.
- That `ConstructInstance` runs after the normal-phase install. Resource construction may happen before the plugin's normal pass
  (the timing question `heaps.md` section 1 answers for the sizes). If it runs earlier, the step is never corrected for those objects.
- The body is not reconstructed in C++. The wrapper runs the game's constructor unchanged. The sub-calls (`0x1028430`,
  `0xffbc50`, `0xfedec0`, the `DefaultInit` guard) are not identified in the disassembly, and reconstructing them would be a guess.
- The source build pairs the step words with R06 (`heaps.md` section 3). This module changes the step alone, as the issue asks.

## Prediction and falsifiers

Prediction (pinned build, a headset run with the plugin loaded in the normal phase): logcat has one
`shaderstep on: CGShaderSetResource table growth step set to 1` INFO line at install, one
`shaderstep: first shader-set table built with growth step 1` INFO line at the first construction, and no `shaderstep: step word is
not the game's 0x20` WARNING. Shader-set tables then grow by one element per Grow.

Falsifiers:
- `module install FAILED` with `status=unexpected_original`: the slot is not 0x365ddc8 or not `base+0x1906f4c` (the vtable reading is wrong).
- `on` INFO, but no `first shader-set table built` line in a session that loads shader sets: the constructor does not run through
  this slot, or it runs before the install.
- `not the game's 0x20` WARNING lines: the constructor is not the one this module reads.

## Headset test (3 lines)

1. Install the build with the plugin packaged; start a session that loads a combat map.
2. `adb logcat | grep shaderstep` — expect the install line and the first-construction line.
3. Put an empty `shaderstep.off` in `/sdcard/Android/data/<pkg>/files/q15t/`, relaunch, expect `shaderstep off` and no wrapper lines.

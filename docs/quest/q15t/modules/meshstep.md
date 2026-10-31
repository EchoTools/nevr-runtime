# Module `meshstep` (issue #1555): mesh-list table growth step 1

Files: `src/quest/plugins/q15t/src/meshstep.{h,cpp}`, `src/quest/tests/q15t/meshstep_test.cpp`, `src/quest/tests/q15t/mod/meshstep.sh`.
CMake option `Q15T_MESHSTEP`, default ON. Kill file `meshstep.off` in the plugin data directory.

## What the stock code does (pinned build b243509c, disassembly `libr15.so`, ELF vaddr = disassembly VA - 0x100000)

- `NRadEngine::CGMeshListResource::CGMeshListResource(CPresenceInit const&)` is ELF 0x18677cc (size 516).
- Its eleven `CTableXT` members are `CMemBlock`s at block offsets 0x2b0, 0x2e8, 0x320, 0x358, 0x390, 0x3c8, 0x400, 0x438, 0x470,
  0x4a8, 0x4e0. Each block's growth step is stored at block + 0x20.
- One `mov w20,#0x20` (ELF 0x186784c, word 0x52800414) is the source of all eleven stores: `str x20` at 0x2d0, 0x308, ..., 0x500.
- The only caller is the GOT thunk at ELF 0x1bcc0 (`adrp x16,0x37e5000; ldr x17,[x16,#0x548]; br x17`): it loads the constructor
  address from the data-page word at ELF 0x36e5548 and branches to it. Two other data words hold the constructor's address
  (disassembly 0x3383ebc, 0x34ebd70); no code reads them (the disassembly has no cross-references to them), so only 0x36e5548 is swapped.
- Callers of the thunk: `ConstructInstance` (disassembly 0x1a05b2c) and `CreateInstance` (0x1a05c00).

## What the module changes

- The GOT word at ELF 0x36e5548 is swapped (data page only, `got_swap.cpp`) from base + 0x18677cc to a C++ reconstruction,
  `BuildMeshList`, which does what the stock constructor does, in the same order, calling the same four helpers
  (`CMemoryContext::Current` through ELF 0xf28430, `CGMeshListData::CGMeshListData` through 0xef9c70, `CMemoryContext::CurrentPtr`
  through 0xefbc50, `CMemBlock::CMemBlock` through 0xeedec0). Its growth step is 1.
- The vtable words the stock code stores first (ELF 0x371f860 + 0x10 for `CPresence`, 0x36fc7d8 + 0x10 for `CGMeshListResource`)
  are read once at install. A zero or unreadable word refuses the module.
- No code word is written. The instruction words the reconstruction was derived from are pinned (`kMeshStepWordPins`): the thunk,
  the two adrp/ldr pairs, the `mov w20,#0x20`, the `ldr q1` constant and all 24 `bl` words of the constructor. A pin that does not
  read back refuses the module with one ERROR line (`word_pin_mismatch`) and nothing is swapped.
- The `{7, 0}` member constant at +0x60 is the value the `ldr q1` loads (ghidra decompile of the constructor shows `this+0x60 = 7`,
  `this+0x68 = 0`); it is written as those two values, not read from rodata.

## Refusals (all loud, nothing swapped)

`meshstep.off` present (INFO, `off_file`); `helper_missing`; `word_pin_mismatch` (ERROR, names the pin); `vtable_unreadable`;
`InstallImportHook` refusals (slot not holding base + ctor, page not writable, etc.; the status is logged).

## Logging

`meshstep on` (INFO, slot_vaddr, growth_step), `meshstep refused` (ERROR), `meshstep off` (INFO), and one INFO on the first
constructed object (`meshstep: first CGMeshListResource built ...`, growth_step 1, stock_growth_step 32).

## Tests

`src/quest/tests/q15t/meshstep_test.cpp` (`mod/meshstep.sh`): a 64 MiB fake image with the pinned words, vtable words and GOT word at
their ELF offsets; the swapped word is called, the object's fields are checked against the stock order, the eleven CMemBlock calls
and step 1 are checked; a mismatched pin, a zero vtable word and a missing helper each refuse with nothing swapped.
Not covered by the host test: the kill file (needs a writable data directory), `wiring_test.cpp` (the module is not in its
option list yet), and the mutant run.

## Prediction and headset check (not yet run on a headset)

On a Quest with the variant APK, a lobby mesh list (a combat map's static instances) builds through `meshstep`:
logcat shows `meshstep on` then `meshstep: first CGMeshListResource built` (growth_step=1), and no `meshstep refused`.
Falsifiers: `meshstep refused ... word_pin_mismatch` (the libr15 build differs from the pins), `vtable_unreadable`, or
`meshstep on` with no `built` line after entering a map that spawns mesh instances (the swap landed but no ctor runs through it).

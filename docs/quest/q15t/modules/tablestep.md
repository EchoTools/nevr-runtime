# tablestep (issue #1552): shader-set table growth step 32 -> 1

Source: `src/quest/plugins/q15t/src/tablestep.cpp`. Host test: `src/quest/tests/q15t/tablestep_test.cpp`.
Option: `Q15T_TABLESTEP` (on by default in `CMakeLists.txt`). Off file: `tablestep.off` in the plugin data directory.

## What the stock code does

ReVault, libr15 (ELF vaddr = ReVault VA - 0x100000), build b243509c08ce677aeb95fa348016949b3fc45230:

- `NRadEngine::CFactory<NRadEngine::CGShaderSetResource>::CreateInstance`, ReVault `0x1a0707c`, 292 bytes (raw decompilation
  and disassembly read in ReVault this session). It allocates a 0xb0-byte object, fills its presence and resource words, runs
  the `CMemBlock` constructor on the CTableXT at object `+0x78`, and then writes the table's growth step.
- The growth step is `mov w8,#0x20` at `0x1a07130` (word `0x52800408`), stored by `str x8,[x19,#0x98]` at `0x1a07138`. The
  table's step is at its `+0x20`, so the object's `+0x98`; the layout matches the `DrawTable` of `custctx.cpp`.
- Three data words hold the address of `CreateInstance` (ReVault xrefs to `0x1a0707c`, `[data]`): `0x3387f9c`, `0x34fb0f8`,
  `0x375ddd0`. Their ELF vaddrs are `0x3287f9c`, `0x33fb0f8`, `0x365ddd0`. ReVault has no code xref to any of the three words
  (`revault_xrefs` `to`, each: none). Whether every caller of the function goes through these words is not shown.

## What the module does

- Pins the code word at ELF `0x1907130` (`0x52800408`) before any slot is touched. A mismatch refuses the module with one ERROR line.
- Swaps each of the three slots with `InstallImportHook` (`import_hook.cpp`), each checked against `base + 0x190707c`. A slot that
  holds something else is refused alone and logged; the others stay installed. Each slot has its own stored original, so a refusal
  does not unhook the others.
- The wrapper calls the stock function, then sets the returned object's `+0x98` from `0x20` to `1` when it still reads `0x20`.
  The stock store is the last write to that field before the return, so the result is the stock function with `mov w8,#1`.
  Any other value is left alone and counted (`unexpected`), with one WARNING line.

The module does not reimplement the allocation or the constructors; that would need further game addresses (the vtables, the
guard, the `CMemBlock` constructor) and adds no behaviour.

## Logging

`tablestep: slot swapped` (INFO, one per slot, `slot_elf_vaddr`, `status`, `reason`), `tablestep: slot refused, its calls stay
stock` (WARNING), `tablestep: on` (INFO, `slots`, `of`), `tablestep: OFF; no slot installed` (ERROR), the first rewrite
(`tablestep: first shader-set table step set`, INFO, once), and the not-stock step (WARNING, once).

## Not shown

- Whether the three slots are writable at install on a headset (`mprotect` on RELRO data). `got_swap.cpp` reports it as
  `kProtectFailed` if not, and the slot line says so.
- The slots `0x3287f9c` and `0x33fb0f8` read back from the pinned file as data that does not hold `0x190707c` (`read_memory`
  this session). The slot `0x365ddd0` reads as zero. A runtime relocation would fill a zero word; a non-zero word that is not the
  stock address is refused by the check, so the log names which slots reach the function on a headset.
- Whether `CreateInstance` runs in the normal phase or earlier; the module installs in the normal phase.

## Headset check (3 lines)

1. `logcat | grep 'tablestep:'`: expect three `tablestep: slot swapped status=ok` lines and `tablestep: on ... slots=3`.
2. Start a level that builds shader sets: expect `tablestep: first shader-set table step set`; no `not the stock value` WARNING.
3. A slot line with `status=unexpected_original` names the slot; its calls keep the stock step (32), as before this change.

# instmodel: CGInstancedModelResource's table growth step 1

Source: `src/quest/plugins/q15t/src/instmodel.{h,cpp}`. Option `Q15T_INSTMODEL` (ON in the source build). Kill file `instmodel.off`
in the q15t data directory. Issue #1554.

## What the stock code does (libr15, pinned build b243509c; disassembly `echovr`, binary `libr15.so`, VA = ELF + 0x100000)

- The constructor `NRadEngine::CGInstancedModelResource::CGInstancedModelResource(CPresenceInit const&)` is at VA 0x1a078dc
  (ELF 0x19078dc), 376 bytes. Its first instruction is `str x20,[sp,#-0x20]!` (word 0xf81e0ff4).
- Its one growth-step constant is `mov w20,#0x20` at VA 0x1a07958 (ELF 0x1907958, word 0x52800414). Every later store reads x20:
  `str x20,[x19,#0x98]` (0x1a07960), then the same at `#0xd0`, `#0x108`, `#0x140`, `#0x178`, `#0x1b0`, `#0x1e8` and `#0x220`
  (0x1a079a0, 0x1a079c0, 0x1a079e0, 0x1a07a00, 0x1a07a20, 0x1a07a3c).
- Those eight fields are the growth steps of eight CTableXT members. Member n has its CMemBlock at 0x78, 0xb0, 0xe8, 0x120, 0x158,
  0x190, 0x1c8, 0x200 (each built by a `CMemBlock` constructor call at 0x1a0790c..0x1a07a38), its step at +0x20 and its capacity
  (zeroed by the constructor) at +0x28. The same layout is `DrawTable` in `src/quest/plugins/q15t/src/custctx.cpp`.
- Callers: the constructor is reached only through a PLT stub at VA 0xfdcb00 (`adrp x16,0x37c5000; ldr x17,[x16,#0xc68]; add x16,x16,#0xc68;
  br x17`), so the GOT word is VA 0x37c5c68 (ELF 0x36c5c68). The stub's callers are `ConstructInstance` (0x1a076dc), `CreateInstance`
  (0x1a077b0) and `FUN_0190772c` (0x1a0772c). the disassembly's data xrefs to 0x1a078dc at 0x338803c, 0x34fb2f0 and 0x37c5c68 are not
  code pointers (0x338803c reads as a two-word record, not an address), so only the GOT word is swapped.

## What changes

The patched binary is the stock one with `mov w20,#1` (0x52800034) at VA 0x1a07958, so all eight steps are 1. A code-page write is not
allowed (`README.md`, "Rules this plugin keeps"), so the module does not patch that word. It swaps the constructor's GOT word for a
wrapper, `InstModelCtorWrapper`:

1. the stock constructor runs (called through `g_real`, the value `InstallImportHook` stores before the swap);
2. for each of the eight member steps, if the field still holds the stock value 0x20, it is set to 1; a field with any other value is
   left alone and named in one WARNING line.

Nothing between the stock constructor's stores reads a step, so the object after the wrapper is the object the patched constructor
builds. The wrapper is a reconstruction of the constructor's effect on the object, not a reimplementation of its body: the body
calls game code (`CMemoryContext::Current`, the `CMemBlock` constructor) and loads two vtable pointers through a GOT entry
(`adrp x8,0x381f000; ldr x8,[x8,#0x860]`), and reproducing those loads by hand would add unverified surface. A full
reimplementation is the follow-up if a headset run shows the wrapper is the wrong shape.

## Install

- Off file `instmodel.off`: INFO `instmodel off`, nothing changed.
- Word pins (`CheckWordPins`): VA 0x1a078dc word 0xf81e0ff4 and VA 0x1a07958 word 0x52800414 must read back, or one ERROR line and
  nothing changed.
- `InstallImportHook` with `expected_vaddr` = ELF 0x19078dc: the GOT word must hold the constructor's address. If the loader has not
  resolved the word yet, the swap is refused (`unexpected_original`) and logged.

## Logging

- `instmodel word swapped` (INFO, fields `hook`, `slot_vaddr`, `real_vaddr`, `status`) once at install.
- `instmodel: CGInstancedModelResource growth steps set` (INFO, `steps_set`, `steps_mismatch`, `step`) per constructed object, capped at 16
  lines per process, then one WARNING that the cap was reached.
- `instmodel: a growth step does not hold the stock value` (WARNING, `offset`, `found`, `expected`) at the first mismatch of an object.

## Tests

`src/quest/tests/q15t/instmodel_test.cpp`: the wrapper against a fake stock constructor, with all eight
steps set, only those bytes changed, the mismatch case and the no-match case. The install path (word pins, the GOT swap) is not covered
by a host test; it runs on a game copy.

## Headset check (3 lines)

1. Install the variant APK with the q15t plugin; logcat should show `instmodel word swapped` once and no `instmodel refused`.
2. Load a combat map; logcat should show `growth steps set` with `steps_set=8 steps_mismatch=0` for the instanced-model objects.
3. Falsifiers: `instmodel refused` or `word_pin_mismatch` (the build differs), or `steps_mismatch` > 0 (the stock value is not 0x20).

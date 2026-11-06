# tablestep: shader sets created by CreateInstance have table growth step 1 in a combat match

Files: `src/quest/combat/tablestep.{h,cpp}`, `tablestep_targets.h`, `table_step.{h,cpp}` (the step correction), `src/quest/tests/tablestep_test.cpp`,
`shaderstep_tablestep_test.cpp`, `src/quest/combat/mutants.d/tablestep.sh`.
Switch: `combat.tablestep` in `config.yaml` (default `true`). The plugin's `tablestep.off` is not read; one found in its data directory is a
Warning (`combat_tablestep_legacy_switch`) that names the key. Runs in the `q15t` and `combat` APKs only (`combat::Active()`).

## What the stock code does (store libr15, build ID b243509c; disassembly VA = ELF vaddr + 0x100000)

- `NRadEngine::CFactory<NRadEngine::CGShaderSetResource>::CreateInstance`, disassembly 0x1a0707c, 292 bytes (ELF 0x190707c). It allocates a
  0xb0-byte object, fills its presence and resource words, runs the `CMemBlock` constructor on the CTableXT at object + 0x78, and then writes the
  table's growth step.
- The growth step is `mov w8,#0x20` at 0x1a07130 (ELF 0x1907130, word 0x52800408), stored by `str x8,[x19,#0x98]` at 0x1a07138. The table's step
  is at its +0x20, so the object's +0x98.
- One data word holds the address of `CreateInstance`: ELF 0x365ddd0, vtable slot 2 of `CFactory<CGShaderSetResource>` (slot 1 is
  `ConstructInstance`, shaderstep.md). `readelf -rW` gives `R_AARCH64_ABS64` there naming the symbol, and it is the only relocation of the store
  libr15 whose target is 0x190707c. The words at 0x3287f9c and 0x33fb0f8, which the disassembly's data cross-references list, have no relocation,
  and 0x3287f9c is not 8-aligned, so they are not slots.

## What the module changes

A thunk on the vtable word (a class C word of ADR 0046), installed by the hook manager (`InstallThunk`, target pinned in `tablestep_targets.h`;
the word must hold base + 0x190707c). The pinned code word `mov w8,#0x20` is read first: a difference refuses the module whole. The handler
always runs the game's function. While a combat map is loaded (see "Scope") it then sets the step of the object the game returned from 0x20 to 1
(`table_step::ApplyShaderSetStep`), which makes the result the stock function's with `mov w8,#1`, since the stock store is the last write to the
step before the return. Any other value is left alone and counted, with one Warning. A null result is returned as it is. The module does not
reimplement the allocation or the constructors.

## Scope (arena shares the APK)

The handler acts only when `table_step::CurrentScope()` says `combat`: a combat map is loaded and no Echo Arena level is
(`combat_level.h` `Classify`, lobby rule off, over the CR15Game the combat tick verified). An Echo Arena level wins over a combat map still
loaded during a transition. In every other scope, and with no game verified yet, the object stays the game's, byte for byte. The scope is read at
each creation and never cached.

For an arena match on the combat APK: the player sees nothing different and the game computes the same. The added work is one scope read per
shader set created (seven `process_vm_readv` calls with one gamespace loaded) on a loading thread, never per frame. What an arena level costs is counted per
level change (`combat_tablestep_counts`); it has not been read on a headset yet.

## Not shown

- Whether every caller of `CreateInstance` goes through the word.
- Whether `CreateInstance` runs after the normal step. A resource created before it is stock.

## Logging

`combat_tablestep_install` (Info; Error for a refusal; a pinned word that differs is the Warning `combat_tablestep_pin` first);
`combat_tablestep_first` (Info, once); `combat_tablestep_not_stock` (Warning, once); `combat_tablestep_stock` (Info, once, `scope`).

## Tests

`src/quest/tests/tablestep_test.cpp` (`just test-quest-combat tablestep`): the real handler through the real thunk and `GotHook` against an image
with the ABS64 word and the pinned code word: step 1 in a combat match, a step that is not 0x20 left alone and warned about once, a null result, every
other scope, the code pin, each refusal, the kill file, the golden lines. `shaderstep_tablestep_test.cpp` installs both factory words in one image.
`src/quest/combat/mutants.d/tablestep.sh` is the mutant list. The relocation, the function and the word are checked against the real file by
`got_pinned_test`.

## When the level is stored

`CncaGameSpace::LoadLevel` (ELF 0x17e5484) stores the level at gamespace + 0x58 (`str x22,[x19,#0x58]` at 0x17e54ec) before it calls
`CResourceInstance::LoadAndInit` (0x17e55d4) and `CArchiveLoader::LoadArchiveAsync` (0x17e56a4), which are where the level's resources come from
[INFERENCE from the call order in this one function; the constructors of those resources were not followed]. Not shown: that the gamespace is
already in CR15Game's tables (which the scope reads) when the store happens, and that no resource of the level is built earlier. If either is
false the modules do nothing in combat with every line green, which is why the headset row below requires `built`/`first`/`steps` lines during a
combat map load.

## Headset check (log only)

On a combat APK: `combat_tablestep_install` with `"result":"installed"` once; entering a combat map, `combat_tablestep_first`. In an Echo Arena
match: `combat_tablestep_stock` with `"scope":"other"` and no `first` line. A refusal names the reason: `original_mismatch` leaves the game's
function in the word.

Required for the headset row: `combat_tablestep_first` during a combat map load; `combat_tablestep_counts` lines at the level changes, with `changed` above 0 for the combat level and `left_alone` for the arena one.

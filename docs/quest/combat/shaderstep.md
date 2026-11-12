# shaderstep: shader sets built by ConstructInstance have table growth step 1 in a combat match

Files: `src/quest/combat/shaderstep.{h,cpp}`, `shaderstep_targets.h`, `table_step.{h,cpp}` (the step correction), `src/quest/tests/shaderstep_test.cpp`,
`shaderstep_tablestep_test.cpp`, `src/quest/combat/mutants.d/shaderstep.sh`.
Switch: `combat.shaderstep` in `config.yaml` (default `true`). The plugin's `shaderstep.off` is not read; one found in its data directory is a
Warning (`combat_shaderstep_legacy_switch`) that names the key. Runs in the combat APKs only (`combat::Active()`).

## What the game does (store libr15, build ID b243509c; disassembly VA = ELF vaddr + 0x100000)

- `CFactory<CGShaderSetResource>::ConstructInstance`, disassembly 0x1a06f4c, size 304 (ELF 0x1906f4c). It builds the object in place and, on its
  common tail, stores the table growth step: `0x1a07000 mov w8,#0x20` then `0x1a07004 str x8,[x19,#0x98]` (the tail is shared by both
  branches of the `cbz x2`).
- The table is the CTableXT at object + 0x78: CMemBlock first, step at table + 0x20 (= object + 0x98), capacity at + 0x28.
- The factory's vtable: the destructor `~CFactory` (disassembly 0x1a06ef0) stores `_ZTV + 0x10` into the object, the address point disassembly
  0x375ddc0. Its entries: 0x375ddc0 -> 0x1a06ef0 (slot 0, `~CFactory`), 0x375ddc8 -> 0x1a06f4c (slot 1, `ConstructInstance`), 0x375ddd0 ->
  0x1a0707c (slot 2, `CreateInstance`, see tablestep.md), 0x375ddd8 -> 0x1a071a0 (slot 3, `CreateData`), 0x375dde0 -> 0x1a071a8 (slot 4,
  `SizeOfInstance`). The wrapped word is ELF 0x365ddc8: `readelf -rW` gives `R_AARCH64_ABS64` there, naming the symbol, and it is the only
  relocation that does.

## What the module changes

A thunk on that vtable word (a class C word of ADR 0046), installed by the hook manager (`InstallThunk`, target pinned in
`shaderstep_targets.h`; the word must hold base + 0x1906f4c). The handler always runs the game's function. While a combat map is loaded (see
"Scope") it then reads the word at object + 0x98 of the object the game returned and, if it is the game's 0x20, writes 1 and reads it back
(`table_step::ApplyShaderSetStep`); any other word is left alone, counted, and warned about for the first five objects. An unreadable object is
no write.

## Scope (arena shares the APK)

The handler acts only when `table_step::CurrentScope()` says `combat`: a combat map is loaded or loading and no Echo Arena level is
(`combat_level.h` `Classify`, lobby rule off, in-flight gamespaces included, over the CR15Game the combat tick verified). An Echo Arena level wins over a combat map still
loaded during a transition (loaded or loading). In every other scope, and with no game verified yet, the object stays the game's, byte for byte. The scope is read at
each construction and never cached.

For an arena match on the combat APK: the player sees nothing different and the game computes the same. The added work is one scope read per
shader set constructed (seven `process_vm_readv` calls with one gamespace loaded, plus two for the in-flight table and three for each fiber in it) on a loading thread, never per frame. What an arena level costs is counted per
level change (`combat_shaderstep_counts`); it has not been read on a headset yet.

## Not shown

- That every shader set is built through this word: the other constructor is `CreateInstance` (tablestep.md).
- That `ConstructInstance` runs after the normal step. A resource built before it is stock. If the image used pointer authentication for vtable
  entries the word would differ and the install would be refused (`original_mismatch`), which is the loud failure.
- The disassembly shows two other tables with the same entries (0x3387f8c/0x3387f94 and 0x34fb0c8/0x34fb0e0). No relocation names their words
  and whether a factory object is ever built through them is not shown; only 0x365ddc8 is hooked.

## Logging

`combat_shaderstep_install` (Info; Error for a refusal); `combat_shaderstep_first` (Info, once); `combat_shaderstep_not_stock` (Warning, the first
five); `combat_shaderstep_stock` (Info, once, `scope`).

## Tests

`src/quest/tests/shaderstep_test.cpp` (`just test-quest-combat shaderstep`): the step correction on a local buffer laid out as the game's object,
then the real handler through the real thunk and `GotHook` against an image with the ABS64 word: step 1 in a combat match with the neighbouring words
unchanged, a word that is not 0x20, every other scope, each refusal, the kill file, the golden lines. `shaderstep_tablestep_test.cpp` installs both
factory words in one image. `src/quest/combat/mutants.d/shaderstep.sh` is the mutant list. The relocation and the function are checked against the real
file by `got_pinned_test`.

## When the level is in the scope

`CncaGameSpace::LoadLevel` (ELF 0x17e5484) stores the level at gamespace + 0x58 (0x17e54ec) and then builds the level's resources
(`CResourceInstance::LoadAndInit` 0x17e55d4, `CArchiveLoader::LoadArchiveAsync` 0x17e56a4, the component systems). The gamespace enters
CR15Game's two tables, which `combat_level` has always read, only after: `CGameSpaceInitFiber::Run` (0x17e4d34) sets the fiber's done bit
(0x17e4d6c) after `LoadLevel` returns, and `UpdateGameSpaceInit` (0x17ecc7c) inserts the gamespace at 0x17ed480 on the path that bit opens. While the
resources are built, the gamespace is in the in-flight table only: rows of init-fiber pointers at game + 0x1ed8, count at + 0x1f08, the fiber's
gamespace at fiber + 0x30. The scope reads it too (`combat_level` `Scan(game, inFlight)`, four pinned instruction words), so a combat map that is
loading counts, and an Echo Arena level that is loading wins. [FACT as read from the code; that the four resource classes are built inside the
window is INFERENCE.] Not shown on a headset: that the fiber table holds the gamespace at the time the resources are built.

## Headset check (log only)

On a combat APK: `combat_shaderstep_install` with `"result":"installed"` once; entering a combat map, `combat_shaderstep_first`. In an Echo Arena
match: `combat_shaderstep_stock` with `"scope":"other"` and no `first` line. Falsifiers: `"reason":"original_mismatch"` (the word is not the
function, so the vtable reading is wrong); an `installed` line with no `first` line in a session that loads shader sets (the function does not run
through this word, or runs before the install); `combat_shaderstep_not_stock` (the constructor is not the one this module reads).

Required for the headset row: `combat_shaderstep_first` during a combat map load; `combat_shaderstep_counts` lines at the level changes (`level_name` says which level ended), with `changed` above 0 for the combat level and `left_alone` for the arena one. The last level of a session has no line, because nothing ends it: load another level after the combat map to get its line.

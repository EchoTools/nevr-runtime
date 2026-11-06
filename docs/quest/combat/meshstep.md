# meshstep: the mesh-list tables grow by 1 in a combat match

Files: `src/quest/combat/meshstep.{h,cpp}`, `meshstep_targets.h`, `src/quest/tests/meshstep_test.cpp`, `src/quest/combat/mutants.d/meshstep.sh`.
Switch: `combat.meshstep` in `config.yaml` (default `false`: the plugin's copy never installed on a headset, so the reconstruction below has not run on one; `true` turns it on). The plugin's `meshstep.off` is not read; one found in its data directory is a Warning
(`combat_meshstep_legacy_switch`) that names the key. Runs in the `q15t` and `combat` APKs only (`combat::Active()`).

## What the stock code does (store libr15, build ID b243509c; ELF vaddr = disassembly VA - 0x100000)

- `NRadEngine::CGMeshListResource::CGMeshListResource(CPresenceInit const&)` is ELF 0x18677cc (size 516).
- Its eleven `CTableXT` members are `CMemBlock`s at block offsets 0x2b0, 0x2e8, 0x320, 0x358, 0x390, 0x3c8, 0x400, 0x438, 0x470, 0x4a8, 0x4e0.
  Each block's growth step is stored at block + 0x20.
- One `mov w20,#0x20` (ELF 0x186784c, word 0x52800414) is the source of all eleven stores: `str x20` at 0x2d0, 0x308, ..., 0x500.
- The only caller is a PLT stub at ELF 0xf1bcc0 (`adrp x16,0x36e5000; ldr x17,[x16,#0x548]; br x17`): it loads the constructor address from
  the JUMP_SLOT at ELF 0x36e5548 (`readelf -rW`: `R_AARCH64_JUMP_SLOT` to 0x18677cc) and branches to it. Two other data words hold the
  constructor's address (disassembly 0x3383ebc, 0x34ebd70); no relocation names them and no code reads them, so only 0x36e5548 is hooked.
- The constructor's callers are `ConstructInstance` (disassembly 0x1a05b2c) and `CreateInstance` (0x1a05c00).

## What the module changes

- A thunk on the JUMP_SLOT, installed by the hook manager (`InstallThunk`, target pinned in `meshstep_targets.h`; the slot must hold
  base + 0x18677cc). No code page and no data page is written by the module; the manager writes the slot.
- While a combat map is loaded (see "Scope") the handler builds the object itself, a C++ reconstruction of the constructor that stores 1 where
  the game stores 32. It does what the stock constructor does, in the same order, with the same four helpers (`CMemoryContext::Current`
  through ELF 0xf28430, `CGMeshListData::CGMeshListData` through 0xef9c70, `CMemoryContext::CurrentPtr` through 0xefbc50,
  `CMemBlock::CMemBlock` through 0xeedec0; each is the target of the `bl` words that are pinned).
- The vtable words the stock code stores first (the data words at ELF 0x371f860 + 0x10 for `CPresence`, 0x36fc7d8 + 0x10 for
  `CGMeshListResource`) are read once at install. A zero or unreadable word refuses the module.
- The `{7, 0}` member constant at +0x60 is what the stock `ldr q1` loads; it is written as those two values, not read from rodata.
- Everywhere else the handler calls the game's constructor and the object is the game's.

## Scope (arena shares the APK)

The handler acts only when `table_step::CurrentScope()` says `combat`: a combat map is loaded or loading and no Echo Arena level is
(`combat_level.h` `Classify`, lobby rule off, in-flight gamespaces included, over the CR15Game the combat tick verified). An Echo Arena level wins over a combat map still
loaded during a transition. The lobbies arena players use, the combat lobby, nothing loaded, and no game verified yet are `other` or `unknown`:
the game's constructor runs. The scope is read at each construction and never cached.

For an arena match on the combat APK: the player sees nothing different and the game computes the same, since the constructor is the game's.
The added work is one scope read per mesh list built (seven `process_vm_readv` calls with one gamespace loaded, plus two for the in-flight table and three for each fiber in it) on a loading thread, never per
frame. What an arena level costs is counted per level change (`combat_meshstep_counts`); it has not been read on a headset yet.

## Refusals (all loud, nothing hooked)

`off_config` (Info); `already_ran`; `module_not_loaded`, `no_build_id`, `build_id_mismatch`; `pinned_word_mismatch` (Warning
`combat_meshstep_pin` with `vaddr`, `have`, `want`, then the Error install line); `helper_missing`; `vtable_unreadable`; and the manager's refusals
(a slot that does not hold the constructor is `original_mismatch`, and so on; the status is the `GotStatus` name).

## Pinned code

`kWords` in `meshstep_targets.h` holds the 33 instruction words the reconstruction was derived from: the PLT stub's `adrp` and `ldr`, the two
`adrp`/`ldr` pairs that load the vtables, the `ldr q1`, the `mov w20,#0x20`, and the `bl` of every helper call. `got_pinned_test` reads each in
the store libr15.

## Logging

`combat_meshstep_install` (Info; Error for a refusal) with `result`, `reason`, `status`, `slot_vaddr`, `consequence`;
`combat_meshstep_built` (Info, once, `growth_step` 1, `stock_growth_step` 32); `combat_meshstep_stock` (Info, once, `scope`);
`combat_meshstep_counts` (Info, one per change of the loaded level, `level`, `scope`, `left_alone`, `changed`: what the level that ended cost).

## Tests

`src/quest/tests/meshstep_test.cpp` (`just test-quest-combat meshstep`): the real handler through the real thunk and `GotHook` against a libr15
image built in memory, with the object checked field by field against the stock order, every other scope checked to leave the game's object
byte for byte, each refusal, the kill file, the golden lines. `src/quest/combat/mutants.d/meshstep.sh` is the mutant list
(`src/quest/combat/mutants.sh meshstep`). The slot, the function and every pinned word are checked against the real file by `got_pinned_test`
(`just test-quest-hooks-pinned`, which needs the store APK).

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

On a combat APK, entering a combat map: `combat_meshstep_install` with `"result":"installed"` once, then `combat_meshstep_built`. In an Echo
Arena match: `combat_meshstep_stock` with `"scope":"other"` and no `combat_meshstep_built`.
Falsifiers: `combat_meshstep_pin` (the libr15 differs from the pins), `"reason":"vtable_unreadable"`, an `installed` line with no `built` line
after a combat map loads (the constructor does not run through the slot, or runs before the install).

Required for the headset row: `combat_meshstep_built` during a combat map load (with `combat.meshstep: true` in `config.yaml` for this row; the key defaults to false); `combat_meshstep_counts` lines at the level changes (`level_name` says which level ended), with `changed` above 0 for the combat level and `left_alone` for the arena one. The last level of a session has no line, because nothing ends it: load another level after the combat map to get its line.

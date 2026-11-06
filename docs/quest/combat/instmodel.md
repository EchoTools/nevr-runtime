# instmodel: the instanced-model tables grow by 1 in a combat match

Files: `src/quest/combat/instmodel.{h,cpp}`, `instmodel_targets.h`, `src/quest/tests/instmodel_test.cpp`, `src/quest/combat/mutants.d/instmodel.sh`.
Switch: `combat.instmodel` in `config.yaml` (default `true`). The plugin's `instmodel.off` is not read; one found in its data directory is a
Warning (`combat_instmodel_legacy_switch`) that names the key. Runs in the `q15t` and `combat` APKs only (`combat::Active()`).

## What the stock code does (store libr15, build ID b243509c; disassembly VA = ELF vaddr + 0x100000)

- The constructor `NRadEngine::CGInstancedModelResource::CGInstancedModelResource(CPresenceInit const&)` is at VA 0x1a078dc (ELF 0x19078dc),
  376 bytes. Its first instruction is `str x20,[sp,#-0x20]!` (word 0xf81e0ff4).
- Its one growth-step constant is `mov w20,#0x20` at VA 0x1a07958 (ELF 0x1907958, word 0x52800414). Every later store reads x20:
  `str x20,[x19,#0x98]` (0x1a07960), then the same at `#0xd0`, `#0x108`, `#0x140`, `#0x178`, `#0x1b0`, `#0x1e8` and `#0x220`
  (0x1a079a0, 0x1a079c0, 0x1a079e0, 0x1a07a00, 0x1a07a20, 0x1a07a3c).
- Those eight fields are the growth steps of eight CTableXT members. Member n has its CMemBlock at 0x78, 0xb0, 0xe8, 0x120, 0x158, 0x190,
  0x1c8, 0x200 (each built by a `CMemBlock` constructor call at 0x1a0790c..0x1a07a38), its step at +0x20 and its capacity (zeroed by the
  constructor) at +0x28.
- The constructor is reached through a PLT stub at VA 0xfdcb00 (`adrp x16,0x37c5000; ldr x17,[x16,#0xc68]; add x16,x16,#0xc68; br x17`), so
  its slot is ELF 0x36c5c68 (`readelf -rW`: `R_AARCH64_JUMP_SLOT` to 0x19078dc). The stub's callers are `ConstructInstance` (0x1a076dc),
  `CreateInstance` (0x1a077b0) and `FUN_0190772c` (0x1a0772c). The disassembly's data cross-references to 0x1a078dc at 0x338803c and 0x34fb2f0
  are not code pointers, so only the JUMP_SLOT is hooked.

## What the module changes

A thunk on the JUMP_SLOT, installed by the hook manager (`InstallThunk`, target pinned in `instmodel_targets.h`; the slot must hold
base + 0x19078dc). The handler always runs the game's constructor. While a combat map is loaded (see "Scope") it then sets each of the eight
steps to 1 where the stock value 0x20 is still there; a step holding another value is left as it is and counted. Nothing between the stock
constructor's stores reads a step, so the object after the handler is the object a constructor with `mov w20,#1` would build. The handler is a
correction of the constructor's effect, not a reimplementation of its body (which calls game code and loads two vtable pointers through a GOT
entry).

## Scope (arena shares the APK)

The handler acts only when `table_step::CurrentScope()` says `combat`: a combat map is loaded and no Echo Arena level is
(`combat_level.h` `Classify`, lobby rule off, over the CR15Game the combat tick verified). An Echo Arena level wins over a combat map still
loaded during a transition. In every other scope, and with no game verified yet, the object stays the game's, byte for byte. The scope is read at
each construction and never cached.

For an arena match on the combat APK: the player sees nothing different and the game computes the same. The added work is one scope read per
instanced-model resource built (seven `process_vm_readv` calls with one gamespace loaded) on a loading thread, never per frame. What an arena level costs is counted per level change (`combat_instmodel_counts`); it has not been read on a headset yet.

## Install

Pinned words (`kWords`): the constructor's first word and the step constant. Refusals, all loud with nothing hooked: `off_config` (Info);
`already_ran`; `module_not_loaded`, `no_build_id`, `build_id_mismatch`; `pinned_word_mismatch` (Warning `combat_instmodel_pin`, then the Error
install line); the manager's refusals (a slot that does not hold the constructor is `original_mismatch`).

## Logging

`combat_instmodel_install` (Info; Error for a refusal); `combat_instmodel_steps` (Info, `steps_set`, `steps_mismatch`, `step`) for each of the
first 16 combat constructions, then one `combat_instmodel_log_cap` Warning; `combat_instmodel_mismatch` (Warning, `offset`, `found`,
`expected`) for each of the first 16 steps that did not hold 0x20; `combat_instmodel_stock` (Info, once, `scope`).

## Tests

`src/quest/tests/instmodel_test.cpp` (`just test-quest-combat instmodel`): the real handler through the real thunk and `GotHook` against a libr15
image built in memory and a stand-in for the game's constructor: all eight steps become 1 and nothing else of the object changes, a mismatching
step is left alone, no stock value means no write, every other scope leaves the game's object byte for byte, the log cap, each refusal, the kill
file, the golden lines. `src/quest/combat/mutants.d/instmodel.sh` is the mutant list. The slot, the function and both words are checked
against the real file by `got_pinned_test`.

## When the level is stored

`CncaGameSpace::LoadLevel` (ELF 0x17e5484) stores the level at gamespace + 0x58 (`str x22,[x19,#0x58]` at 0x17e54ec) before it calls
`CResourceInstance::LoadAndInit` (0x17e55d4) and `CArchiveLoader::LoadArchiveAsync` (0x17e56a4), which are where the level's resources come from
[INFERENCE from the call order in this one function; the constructors of those resources were not followed]. Not shown: that the gamespace is
already in CR15Game's tables (which the scope reads) when the store happens, and that no resource of the level is built earlier. If either is
false the modules do nothing in combat with every line green, which is why the headset row below requires `built`/`first`/`steps` lines during a
combat map load.

## Headset check (log only)

On a combat APK: `combat_instmodel_install` with `"result":"installed"` once; entering a combat map, `combat_instmodel_steps` with
`steps_set` 8 and `steps_mismatch` 0. In an Echo Arena match: `combat_instmodel_stock` with `"scope":"other"` and no `steps` line.
Falsifiers: `combat_instmodel_pin`, or `steps_mismatch` above 0 (the stock value is not 0x20).

Required for the headset row: `combat_instmodel_steps` during a combat map load; `combat_instmodel_counts` lines at the level changes, with `changed` above 0 for the combat level and `left_alone` for the arena one.

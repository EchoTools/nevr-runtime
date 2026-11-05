# equiprefire (issue #2121): an equipment change the scripts dropped is applied again once the slot is still

Source: `src/quest/plugins/q15t/src/equiprefire.cpp`. Host test: `src/quest/tests/q15t/equiprefire_test.cpp`.
Option: `Q15T_EQUIPREFIRE` (on by default in `CMakeLists.txt`; always on, no config key). Off file: `equiprefire.off` in the plugin data
directory. Declares `NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY` (the game's change handler runs once more, so the slot's scripts act on the last choice).

The decision rule is shared with the PC build and is not repeated here: `src/runtime/patch/equipment_refire_rules.h` (the debounce, `kSettleMs`
2500 ms, the reads that find a record, its actor key and the local player's slot handle). The PC glue is `src/runtime/patch/equipment_refire.cpp`.

## What the stock code does

Disassembly, libr15 (ReVault VA; ELF vaddr = VA - 0x100000), build b243509c08ce677aeb95fa348016949b3fc45230:

- `CR15NetPlayerEquipmentCS` keeps one 0x58-byte record per player (index table `+0xc8`, records `+0xf8`, link table `+0x100`). Three setters
  take (component `x0`, handle `x1`, value `x2`) and write the slot only when the value differs: `SetWeaponSlot` `0x227ca84` (record `+0x18`,
  `ldr x9,[x8,#0x18]!` at `0x227cab8`), `SetAbilitySlot` `0x227cde4` (`+0x20`, `0x227ce18`), `SetGrenadeSlot` `0x227cf90` (`+0x28`, `0x227cfbc`).
  Each ends `cmp x9,x2; b.eq <return>` and otherwise `str x2,[x8]` (`0x227cac4`, `0x227ce24`, `0x227cfd0`).
- The change handlers read the value from the record and send the equipment-changed event: `HandleWeaponChange` `0x227afb4`,
  `HandleAbilityChange` `0x227b124`, `HandleGrenadeChange` `0x227b1dc`. The weapon and ability setters inline the handler's work;
  `SetGrenadeSlot` calls `HandleGrenadeChange` (through the PLT stub `0xfd7640`, `bl` at `0x227cfd4`).
- **The three handler addresses are the re-fire targets. The hook points are the setters.** The order that asked for this module calls the
  handlers "Quest rows"; the module hooks the setters (`0x227ca84`, `0x227cde4`, `0x227cf90`) and calls the handlers.
- What the record's `+0x10` and `+0x14` words gate (read in this session, and the earlier note that the handler "runs only when" they are set
  was wrong):
  - `HandleAbilityChange` (`0x227b124`): `ldr w8,[x8,#0x10]` at `0x227b154`, `cbz w8,0x227b178` skips only `bl 0xff81d0` at `0x227b174`, the stub of
    `RegisterExclusiveUpdate` (`0x19063f4`). `0x227b178..0x227b1d8` then reads the component and ends `b 0x104a800`, the stub of
    `CncaGameSpace::SendComponentEventGlobal` (`0x190735c`), with no condition. `HandleWeaponChange` (`0x227b004`, `0x227b068`) has the same shape.
    In `HandleGrenadeChange` the same `+0x10` test (`0x227b20c`, `cbz w8,0x227b368`) skips `RegisterExclusiveUpdate` and the block after it
    (`0x227b230..0x227b364`, which compares the ordnance value against a table of ids and stores a result; its purpose was not traced); the send at `0x227b3c8` is reached either way.
  - `SetAbilitySlot` (`0x227cde4`): `+0x10` (`ldr w8,[x27,#0x10]!` at `0x227ce30`, `cbz` at `0x227ce3c`) gates only `bl 0xff81d0`
    (`RegisterExclusiveUpdate`, `0x227ce58`). `0x227ce5c..0x227ceb0` ends `bl 0x104a800` (`SendComponentEventGlobal`) unconditionally. After it,
    `+0x10` (`0x227ceb4`, `cbnz` to `0x227cecc`) and `+0x14` (`0x227cec4`, `cbz` to the return `0x227cf74`) gate a tail
    (`0x227cecc..0x227cf70`) that reads the net game (`0x103a070` stub), `UserId` (`0x1044c30` stub) and calls the stub at `0xfe3e60`, which is
    `Broadcast<SR15NetPlayerEquipmentUpdate>` (`0x2404244`): the network update, not the event. `SetWeaponSlot` has the same layout
    (`0x227cad0..0x227cb68`, event at `0x227cb50`, tail `0x227cb6c..0x227cc10`); `SetGrenadeSlot` calls the handler at `0x227cfd4` unconditionally
    and gates the same tail (`0x227cfe0..0x227cff4`).
  - So a re-fire of a handler sends the equipment-changed event whatever `+0x10` and `+0x14` hold; they decide whether the exclusive update is
    registered and whether the update is broadcast. This is the same statement as the PC glue's: there the two compares gate
    `DeserializeSyncFields`, and the handler call is unconditional. The shared header has no gate for them, and none was added.
- The data words that hold each setter's address (ReVault `[data]` xrefs, all read): per setter a `JUMP_SLOT` (the PLT stub's GOT slot) and a
  `GLOB_DAT` word. ELF: weapon `0x36f3d00` and `0x37233e8`, ability `0x36fa900` and `0x3727b38`, grenade `0x36d8e10` and `0x3711db8`. The PLT
  stubs are `0x1038c30`, `0x1046430`, `0x1002e50` (`adrp x16; ldr x17,[x16,#0xd00 / #0x900 / #0xe10]; br x17`). The other xrefs ReVault lists are
  not slots: `0x242e830` is inside `Enter @ 0x242e5c8` and is a bogus xref to all six names; the seven code addresses `0x221cdac`, `0x221cfdc`,
  `0x221c0a4`, `0x221d794`, `0x24def84`, `0x24df344`, `0x24df524` all read `00 80 80 3c` (`stur q0,[x0,#8]`), not a load of a GOT word;
  `0x33c551c`, `0x33c5534`, `0x33c553c` are 4-aligned pair tables that cannot hold a pointer; `0x35e7268`, `0x35e72e0`, `0x35e7310` are
  `.eh_frame` data. The relocation kinds themselves (JUMP_SLOT versus GLOB_DAT) are the order's and the same pattern as `powerboost`'s `Heal`
  words; they were not read with `readelf`.
- The local player: `CR15NetGame::UserId` `0x135a9b8` (`x0` game, `x1` actor key 0 = `*(ctx+0x58)`, `x2` actor key 1 = the record's link) returns
  the slot handle of the player with that key; the net game comes from `NetGameMT` `0x13027d4` (`mov w8,#0x8510; ldr x0,[x0,x8]; ret`) called with
  `*(ctx+8)`. The local player is the net game's slot 0 (`LocalSlotHandle` in the rule header). Both are called by their pinned addresses.

## What the module does

- Pins 24 code words (`kEquipRefireWordPins`, `equiprefire.h`) before any word is touched: each setter's entry and its three reads, each
  handler's entry and index-table read, `NetGameMT` whole, and the first words of `UserId`. A mismatch refuses the module with one ERROR line
  (`code word pin mismatch`, with pin / found / expected) and reason `word_pin_mismatch`; nothing is swapped. No MD5 or file-hash gate.
- Swaps six data words with `InstallImportHook`, each checked against `base + <setter vaddr>`, each with its own stored original (a refused word
  clears only its own). A word that holds something else is refused alone (WARNING `slot refused, its calls stay stock`); the others stay installed.
- Each wrapper reads the record's value before the original runs, calls the original with the same three arguments, and then (a) works out
  `changed = before != value`, (b) asks whether the record is the local player's, only for a change, and (c) feeds the shared `Debouncer`
  under a mutex. The vtable of the component is kept with the tracked change.
- `EquipRefireTick`, registered with `AddGameTick` only when the install succeeded, runs on the game thread each frame. For each slot whose last
  change is older than `kSettleMs` it re-reads the component (readable, same vtable as when the change was tracked, record still named by the
  handle, value still the one tracked, still the local player's) and calls `handler(component, handle)`: the handler, never the setter, so a
  re-fire cannot start another debounce. A check that fails is a skip with a logged reason (`component_unreadable`, `component_replaced`,
  `record_gone`, `record_changed_elsewhere`, `not_local`).
- Hook frequency: the setters run once per loadout change (a button press, or the game restoring a loadout), not per frame or per tick; the tick
  does one lock and one empty-table test per frame.
- A dedicated server has no Quest build; the rule header's `server` verdict is not used here.

## Logging

All lines are q15t plugin lines: `[NEVR.PLUGIN] plugin=q15t equiprefire: ...` (a plugin cannot choose its tag, `docs/standards/logging.md` Rule 15).
The field names are the PC `[NEVR.EQUIP]` line's.

- `equiprefire: slot swapped` (INFO, one per word: `slot`, `word` = `jump_slot` or `glob_dat`, `slot_elf_vaddr`, `fn_elf_vaddr`, `status`, `reason`),
  `slot refused, its calls stay stock` (WARNING), `OFF; no slot installed` (ERROR), `equiprefire: on` (INFO: `slots`, `of`, `settle_ms`, and the six
  word vaddrs per slot), `equiprefire off` (INFO, kill file).
- `equiprefire: equipment change` (DEBUG, one per setter call, at most 256 per process, then one WARNING `per-call records capped`): `slot`
  (`weapon`, `tac_mod`, `ordnance`), `word`, `verdict` (`tracked`, `unchanged`, `not_local`), `handle`, `value`, `read`, `changed`, `local`.
- `equiprefire: equipment re-fired` (INFO): `slot`, `value` (hex), `handle`, `changes`, `quiet_ms`.
- `equiprefire: equipment re-fire skipped` (INFO): `slot`, `value`, `handle`, `reason`.

The Quest value is hex: the Quest build has no symbol-name corpus to name a CSymbol64.

## Prediction (written before any headset run)

Rapid A, B, C at the station (tac mod): after the last press and 2.5 s of stillness the log has
`equiprefire: equipment re-fired ... slot=tac_mod value=<C> handle=0x... changes=3 quiet_ms=2500+` (the tick runs each frame, so `quiet_ms` is a
frame or two above 2500), and the ability that fires is C. Falsified by: `verdict=not_local` at DEBUG for the player's own change (slot 0 is not
the local player on Quest), no `verdict=tracked` lines at all, a re-fire line with a value that is not the last choice, or B still firing
after the re-fire (then 2.5 s is too short, or the handler is not the repair).

## Not verified

- No headset run. Nothing here ran against the game; the host test uses a fake component and fake setters, handlers and lookups.
- `kSettleMs` (2500 ms) on Quest: the value is the rule header's, measured on PC by the community tool; the first headset log's `quiet_ms` and
  the order of the lines are the measurement.
- The relocation kinds of the six words (`readelf -rW` of the pinned library was not run); the words were taken from ReVault's `[data]` xrefs.
- That the local player is the net game's slot 0 on Quest (the rule's assumption, from `UserId` and the PC `SetTier`); it fails closed.
- The event id the handlers send (`0xefe2dea238cf2e32`, the rule header's) was not re-read in this session; the handler bodies were.
- That `HandleWeaponChange` and `HandleGrenadeChange` are repairs as well as `HandleAbilityChange` (the reported drop is the ability core's; weapons
  and ordnance are a tester's report).
- The `+0xf2` record count the rule's `DenseIndex` reads: no `CR15NetPlayerEquipmentCS`-specific reader was found; only the shared component
  storage code reads it.

## Headset check (4 lines)

1. `logcat | grep 'equiprefire:'`: expect six `slot swapped` lines and `equiprefire: on` with `slots=6 of=6`.
2. At the station, change the tac mod quickly three times and wait 3 s: expect one `equipment re-fired` line with `slot=tac_mod changes=3`.
3. Note which ability fires; with the re-fire it is the last one chosen.
4. `touch equiprefire.off` in the plugin data directory and restart: expect `equiprefire off` and no re-fire.

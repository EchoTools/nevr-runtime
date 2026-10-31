# Where the game keeps its main level

`NvrGameContext.level_symbol` and `level_name` (plugin API v8, `sdk/include/nevr/plugin_interface.h`) carry the level the
game holds as its main level. This pins where the loader reads it, what proves that field, what a client run showed, and
what the evidence does not cover. Evidence is the disassembly (project `echovr`, `echovr.exe` and `libr15.so`; Quest VAs are
the disassembly's, pinned vaddr + `0x100000`) and client runs of a build with a test plugin.

## The field

The game context (the runtime's `g_pGame`; the net game's second field `*(*(g_pGame + 0x8518) + 8)` is the same
address) holds the main level as a CSymbol64 qword. `-1` means none, and the runtime reads `-1` as "no level" (`0`).

| | PC (`echovr.exe`) | Quest (`libr15.so`) |
| --- | --- | --- |
| Main level symbol | context `+0x8a48` (`kPcMainLevelSymbolOffset`) | game `+0x8a40` (`kQuestMainLevelSymbolOffset`) |
| Accessor | `0x140100d90`: `MOV RDX,[RCX+0x8a48]; JMP 0x1404f3600` (finds a game space by symbol) | `CurrentSpawnGameSpace` `0x130065c`: `mov w8,#0x8a40; ldr x1,[x0,x8]; b 0x01052840` |
| Callers of the accessor | `CR15SpawnPointCS::CycleSpawnPoint` `0x1408a52c0` and two other spawn-point functions | `CR15SpawnPointCS::CycleSpawnPoint` `0x1e03688` (it was not shown to call the accessor itself) |
| Serialised by | `CR15Game` vslot 91 (`0x1401064b0`): `+0x8a48` and `+0x8a50`, 8 bytes each | `CR15Game::GameSaveLoad` `0x13003e0`: `+0x8a40`, 8 bytes |
| Set to `-1` | `0x1400f6130`: `MOV qword ptr [RDI+0x8a48], -1` | `CR15Game` constructor `0x12fce34`: `*(this+0x8a40) = -1` |
| Written with a game space's symbol | not located (the only direct stores are the `-1` reset) | `SetCurrentSpawnGameSpace` `0x12fbcf8`: compare-and-swap of `[gamespace+0x58]` into `+0x8a40` |

The Quest name says what the field is: the symbol of the current spawn game space, the level the players spawn in.
`src/abi/game_level.h` holds the offsets and the read; `ContextFor` in `src/runtime/ext/plugin_loader.cpp` fills the
fields in every init, per-frame and state-change context, and logs `[NEVR.PLUGIN] main level changed: from=<> to=<>
symbol=0x<hex>` once per change.

## What a PC client run showed

A client started with `-windowed -mp` on a game copy, with a plugin that logs the level on every state change
(`src/runtime/tests/plugin_level_test_dll.cpp`), logged in and joined the social lobby:

| Moment | State | Main level field | `[LEVELLOAD]` lines |
| --- | --- | --- | --- |
| boot, login | loading root, logging in, logged in | none | `mnu_master`, `r14_glb_global_root` |
| after login | loading global, lobby | none | `r14_glb_global_mp` |
| entering the lobby level | lobby → loading level | `mpl_lobby_b2` (`0xd09afd15b1c75c04`), already set in the state-change context | `mnu_master_mp_ingame`, `mpl_lobby_b2`, `mpl_lobby_b_arena`, `mpl_lobby_b_combat` follow it |
| lobby ready, in game | ready for game, in game | `mpl_lobby_b2` | none |

So the field is set when the game enters "loading level", before that level's `[LEVELLOAD]` line, it stays at the
lobby's level while the lobby's arena and combat sub-levels load, and it is none while only the menu and global levels
are loaded.

A scan of the context for the symbols of every level the game logged found three direct level qwords: `mnu_master` at
`+0x9d8`, `r14_glb_global_mp` at `+0x8588` and `mpl_lobby_b2` at `+0x8a48`. The first two are other level slots whose
roles this doc does not pin.

## The other field, which is not the main level

The game object `CncaGame` (the base of `CRxGame`, at offset 0) has a qword pair at `+0x7678` (symbol) and `+0x7680`
(handle), written by `CncaGameSpace::LoadLevel` (`0x1404fe339`) only when the handle slot is `-1` and the level
resource's handle (`[resource+0x20]`) is not `-1`, by the game-space update `0x1404efd00`, by the `-level` seed
`0x140504790`, and reset to `-1` by `0x14050aad0` and `0x14050b2a0`. In a multiplayer client run it held `-1` through
every level above, so it does not follow the main level. `GameSaveLoad` serialises it with the game state.

## Questions the issue asked

- **Can a level change without a game-state transition?** The runs show the field change only inside the transition into
  "loading level". A change without a transition was not observed, and no code path for one was found. The per-frame
  context carries the level as well, so a plugin that reads it in `OnFrame` misses nothing if a build does it.
- **Custom levels.** A level the symbol cache (`src/runtime/log/symcache.h`, generated data) does not name, such as a
  plugin's `mpl_arena_dodgeball`, has `level_symbol` set and `level_name == nullptr`. A plugin compares `level_symbol`
  with the CSymbol64 of the name its config gives it (`sdk/include/nevr/level_gate.h`).

## Not shown

- A match level (an arena or combat match) was not reached: a client alone stops in the social lobby. That the field
  follows a match's level is shown by the static uses above (the spawn code looks its game space up by it), not by a run.
- Quest: the offset and its uses are static only; no Quest run read it. The Quest plugin loader reads it through the frame
  hook's netgame (`src/quest/sentinel/main_level.cpp`) and gives it to plugins as `level_symbol` and `level_name` (#1106);
  that path is not yet shown by a headset run.
- Who writes the PC field with a symbol is not located. The runs show when it changes, not which function stores it.

## Falsifier

A PC client in the social lobby whose log shows `main level changed` naming a level other than the one the game entered
"loading level" for, or no change line at all, or `level_symbol == 0` in the state-change context after `loading level`.

# The drone explosion's page fields

The drone explosion reads four words of the EC page's drone block (`re33_ec_page`, page+0xd60): a counter, the context of the last drone
node, and the spawn and sound entries the drone library's prelude stores there. This document records what the built library stores, what the
name of the drone's sound key is, and what is not established.

## Origin

The reference build is `kotorvr/echoquest-combat` at `c5a67e8`. The facts come from these files of that build:

- `runtime/native/afkxr/afk_dronekill_core.h`: the drone actor, the damageable and the sound key as symbol hashes (`kDroneActor`, `kDamageable`,
  `kDroneSound`, labelled DRONE_SOUND), and the spawn and sound offsets (`kDroneCtx`, `kDroneSpawn`, `kDroneSoundFn`).
- `runtime/native/afkxr/afk_dronekill.cpp`: the explosion's use of the page, and the damageable's ON Enter.
- `libsrc/221a0a3a2e56d247/221a0a3a2e56d247.cpp`: the lifted prelude of the drone library (the stores at the drone block, the two entries).

Our copies: `src/quest/combat/ec_core.h` (the offsets, next to `kDroneBlock`) and `src/quest/combat/check/drone_explosion_page.sh` (the check of
the built library). The PC port reads the same words in `src/runtime/combat/dronefix.cpp` (`DEATH_TOKEN_RVA`, `DroneCtx`, `SpawnDeathFx`,
`DRONE_SOUND`) and its damageable key in `src/runtime/patch/dronekill_rules.h`.

## What was established

Measured in the library that `just build-quest-combat-libs` builds from that libsrc (`lib221a0a3a2e56d247.so`, NDK r21b, disassembled with the
NDK's `llvm-objdump`). The prelude computes the drone block from the page pointer in `x0` (`add x9, x0, #0xd60`) and stores:

| Field | Page offset | Store (address in the built library) | Value |
|---|---|---|---|
| counter (node steps) | +0x18 | `add x8, x0, #0xd78` (8b84), then `ldaxr`/`stlxr` on `[x8]` (8bb4) | incremented by one |
| context of the last node | +0x20 | `add x1, x9, #0x20` (8bd0), `str x19, [x0]` (8c0c) | `x19` loaded from the stack (`ldr x19, [sp, #0x28]`) |
| spawn entry | +0x28 | `add x1, x9, #0x28` (8c28), `str x19, [x0]` (8c60) | `0xb8d0`, the symbol `re33_ecent_221a0a_13720` |
| sound entry | +0x30 | `add x1, x9, #0x30` (8c7c), `str x19, [x0]` (8cb8) | `0xbb14`, the symbol `re33_ecent_221a0a_13920` |

The stores go through the library's window write (a `bl` between the address and the store). The check `src/quest/combat/check/drone_explosion_page.sh`
asserts this sequence against the offsets in `src/quest/combat/ec_core.h`.

The drone's sound key, `0x5b8cc538e22ad937`, is `CSymbol64("ncaPhysics")`. The transliteration of the hash in `sdk/include/nevr/symbol_hash.h`
reproduces the four vectors in that header. The repository's symbol table names the same value `ncaPhysics`
(`src/runtime/log/symcache_data.cpp`). The upstream and `DRONE_SOUND` in `dronefix.cpp` call this key the drone's sound key.

The damageable key, `0xc9dc08cfc2f5920f`, is `CSymbol64("R14NetDamageable")`. In `echovr.exe`, the PC port's enable-enter hook is the function at
`ENABLE_ENTER_RVA` in `dronefix.cpp`, and its prologue bytes match the function at that address in the game (checked in ReVault).

PC constants for the same words, by symbol name in `src/runtime/combat/dronefix.cpp`: `DEATH_TOKEN_RVA` (0x2104D60, the same base as the
prelude's drone block), `+ 0x20` in `DroneCtx`, slot 0 at `+ 0x28` in `SpawnDeathFx`, and slot 1 at `+ 0x30` in the loop of `SpawnDeathFx`. The
counter at +0x18 has no PC constant.

## Not established

- Which script is the drone script. The key `0x5b8cc538e22ad937` occurs as an immediate in eight script DLLs of the game and as a component key in
  `echovr.exe`; none of them was identified as the drone's script.
- The symbol the sound entry uses. The lifted source of `re33_ecent_221a0a_13920` loads `0xc8c33e48297612b6`. That value is not
  `CSymbol64("POINT")`, which is `0xe32dc7cad6864aa4`; the upstream calls the entry "POINT's sound" without a symbol.
- The PC kDamageable branch: that the PC enable-enter hook (`Hooked_EnableEnter` in `dronefix.cpp`) takes the damageable branch for
  `dk::kDamageable` the way the upstream's `afk_dronekill.cpp` does.
- That the context at +0x20 is the drone node's context at run time. The lifted source (`rbx = rcx` at entry) and the stack slot give it;
  no run of the library was made.
- Whether the key `0x5b8cc538e22ad937` is used in the drone's graph as a sound system or as the physics system its name gives. The name is
  `ncaPhysics`; the role in the drone's graph is open.

# Module `pfx` (the particle-effect renderer's gate)

Files: `src/quest/combat/pfx.{h,cpp}`, `src/quest/tests/pfx_test.cpp`, `src/quest/combat/legacy_switch.h`. The module is a normal combat step
(`src/quest/combat/combat_install.h`): it runs in the `q15t` and `combat` APKs and in no other. Switch in `config.yaml` (docs/adr/0046 rule 4):
`combat.pfx`, true unless the file says false.

Origin: the gate reconstructs the renderer switch of kotorvr's echoquest-combat repository (commit c5a67e8): `On()` at native/afkxr/afk_pfx.cpp:6958, and the
level gate at runtime/native/afkxr/afk_pfx.cpp:1943-1955. The plugin's version (#1682) was reconstructed from it and is the one this module replaces.

## What the gate decides

- The reference build's `On()` is off unless a switch file `pfx.on` exists in its data directory, or a debug property reads `1`. The working build has the
  renderer on, so the gate is on by default and `combat.pfx: false` turns it off.
- The gate installs no hook, registers no tick and writes no byte. The install step stores the decision and writes one line,
  `combat_pfx_install` (`state` on or off, `reason` default or off_config, `switch` `combat.pfx`, `scope` `combat_map`).
- `GateOn()` reads the stored decision and reads off until the install step has run. A `pfx*` module does nothing while it is off.
- The plugin's kill file `pfx.off` is no longer read. One found in the plugin's data directory is a Warning, `combat_pfx_legacy_switch`, naming the
  key that replaces it.

## Arena

Arena and combat run on the same APK, so the APK does not say which one a player is in. The gate a consumer asks is `GateOpenIn(game)`: open only while
the switch is on and the loaded levels hold a combat map and no Echo Arena level (`combat_level.h`). The combat lobby and the lobbies arena players
use are shut. The plugin's gate was on in every level; nothing read it, so no behaviour changes for arena, and the scope is what keeps a future renderer
from running in an arena match.

`pfx_test` shows the gate open on the four combat maps and shut on `mpl_arena_a`, `mpl_arena_a_lowspec`, `mpl_arena_b`, `mpl_tutorial_a`, the lobbies
(`mpl_lobby_b`, `mpl_lobby_b2`, `mpl_lobby_b2_lowspec`, `mpl_lobby_b_social`, `mpl_lobby_b_arena`, `mpl_lobby_b_combat`), a transition that still holds an arena level, an unreadable game and
the switch off.

## Cost (ADR 0038)

No frame runs this module: the install step runs once per process and no caller of `GateOn` or `GateOpenIn` exists in the tree. A caller takes
`GateOpenIn` (or `InCombatMap`) at a level change and keeps the answer; `InCombatMap` is the level scan, a few `process_vm_readv` reads, and is not for
a per-frame or per-create call: the renderer creates about 3,450 effects while a lobby loads, so its caller takes the cached map-only answer of combat_scope
(#2246 adds `InCombatMap()` there), never this scan. `pfx_test` prints the measured host cost of both calls; on the machine that built this change `GateOn` took about 10 ns and `InCombatMap` about
3 µs per call (x86 host, in an arena level, not a headset).

## What is not ported

The gate is the only part of the switch this module reconstructs. Each of these is needed before the Quest draws the particle effects the working build
draws:

- The renderer object: the reference build creates one `CGParticleEffectRenderer`, publishes it in the engine's global, and drives its Initialize,
  OnSimulation, UploadConstants, UpdateFinished and Render calls, which the Quest build never calls.
- The compute passes (Update, two Emission passes, UpdateBillboards) with their per-resource state, and the billboard draw.
- The CreateEffect wrapper on the engine's JUMP_SLOT, the effect-id tier switches (`pfxt*`), and the warm-up passes (`pfxwarm`, `pfxmeshwarm`). Each
  `pfx*` row of the parity table is its own issue (#1632 to #1671).
- The shader and pack assets the renderer loads from the content pack.

Until those land, turning the gate on changes nothing in the game. The `combat_pfx_install` line is the only observable effect.

## Not shown

- That the renderer's gate is the only thing the working build reads for `pfx`.
- That the pinned build's engine global the renderer publishes is free on the Quest. No address is pinned here.

## Headset test

1. A combat APK (or a development APK repacked with the `combat` variant word, or `combat.force: true` in `config.yaml`); start the game.
2. The sentinel log holds one `combat_pfx_install` line with `"state":"on","reason":"default"`, and the `combat_install_module` record for `pfx` has `"outcome":"installed"`.
3. Set `combat: {pfx: false}` in `config.yaml`, relaunch: `"state":"off","reason":"off_config"`.

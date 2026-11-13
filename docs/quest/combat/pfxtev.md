# Module `pfxtev` (the event-effect decision)

Files: `src/quest/combat/pfxtev.{h,cpp}`, `src/quest/tests/pfxtev_test.cpp`, `src/quest/combat/legacy_switch.h`. A normal combat step, in the combat
APKs only. Switch in `config.yaml` (docs/adr/0046 rule 4): `combat.pfxtev`, true unless the file says false.

Origin: the id list and the switch's place in the renderer come from the event tier of kotorvr's echoquest-combat repository (commit c5a67e8): `kTierIds` at
native/afkxr/afk_pfx.cpp:1914 (the event row at :1915-1916) and the level gate at runtime/native/afkxr/afk_pfx.cpp:1943-1955, read as text; the kill switch there
is `pfxtev.on`, a test-only switch that the working build ships on. Nothing is copied: the ids are six symbol hashes, each the CSymbol64 of an effect name, and the
decision is written here.

## What it decides

Six effect ids (`kIds`) are GPU events: an effect on an events graph that spawns child effects. `AllowsIn(id, inCombatMap)` answers whether
the renderer may create one:

- an id that is not one of the six: allowed (this switch does not gate it), and no line;
- one of the six with the switch on: allowed, one `combat_pfxtev_decision` line with `id`, `result` and `reason`;
- one of the six with `combat.pfxtev: false`: refused, the same line;
- one of the six outside a combat map (`AllowsIn(id, false)`): refused whatever the switch says, `reason` `level`.

The switch is on by default. The install step stores it and writes one `combat_pfxtev_install` line. Nothing calls the decision without the level. Past 64 decision lines per process one Warning,
`combat_pfxtev_log_cap`, is written and the rest are only counted (`CurrentStats()`). The plugin's kill file `pfxtev.off` is no longer read; one found in
the plugin's data directory is a Warning, `combat_pfxtev_legacy_switch`. (The plugin never called its own `InitPfxTev`, so that file changed nothing
before either.)

## Arena

The six ids are combat effects (`pfx_test_combat_plrdeath_*`). `AllowsIn` takes `inCombatMap` from `pfx::InCombatMap(game)` at a level change, so an
arena match, which loads none of them, never has one created. `pfxtev_test` shows an event id allowed on a combat map and refused outside one, and a
non-event id allowed everywhere.

## Cost (ADR 0038)

No frame runs this module: the install step runs once, and nothing calls `Allows` or `AllowsIn` yet (the renderer is #1631). A decision is a scan of six
words, an atomic load and, up to the cap, one log line. `pfxtev_test` prints the measured host cost of a decision.

## Not yet wired (#1631)

Nothing calls `Allows` or `AllowsIn`. The renderer that creates effects is the `pfx` gate's consumer (`docs/quest/combat/pfx.md`), which is not in the
runtime. Until #1631 lands, the switch has no effect on a headset.

## Not observed on a headset

- That the six ids are the ids the pack's event effects use (not checked against the pack on this tree).
- That the renderer, once it asks, creates these effects with the switch on and draws their children.
- Prediction, written before the run: on a combat APK, with `combat.pfx` and `combat.pfxtev` on, the log holds `combat_pfx_install` and
  `combat_pfxtev_install` with `"state":"on"`, and nothing else from either module (nothing asks).

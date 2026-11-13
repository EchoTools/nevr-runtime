# ADR 0002: the level refresh-rate rule is on by default, for combat maps only

Status: accepted (#1737); amended for arena (#2172). Supersedes the default in ADR 0001; ADR 0001's timing analysis stands.

## Context

#1737 asks for every behaviour the reference build gives a Quest player. That build's final shipped on-list turns on two
refresh-rate files: `hzlobby90.on` and `hz72q2.on` (the source build's hz module, header comment). Its rule:

- the level rate while a combat map (`mpl_combat_dyson`, `_combustion`, `_fission`, `_gauss`) or a lobby level
  (`mpl_lobby_b`, `_b2`, `_b2_lowspec`, `_b_combat`, `_b_arena`, `_b_social`) is loaded;
- the game's own rate (120 on a Quest) while an Echo Arena level (`mpl_arena_a`, `_a_lowspec`, `_b`, `mpl_tutorial_a`) is
  loaded, which wins over a lobby level still loaded during a transition, and in every other state;
- the level rate is 90, or 72 on a Quest 2 (`ro.product.device` is `hollywood`).

ADR 0001 showed the game clock is variable-step, so this is a performance setting, not a timing-parity one. It also records that the
lobby holds 118-120 Hz at 2-4 ms of app time; the upstream's 90 in the lobby was a tester's preference, not a measured need.

## Amendment (#2172): arena keeps its rate

Arena and combat run on the same APK, and `mpl_lobby_b2` is the ordinary lobby every arena player uses, so a rule that lowers every lobby level
lowers arena's lobby from 120 to 90 Hz. This rule departs from the upstream's: the lobby levels other than the combat lobby (`mpl_lobby_b_combat`)
never change the rate, and the combat lobby does only with `combat.refresh_rate_lobby: true`, which defaults to **false**. An Echo Arena level
always keeps the game's rate.

## Decision

The rule is on by default and lives in `src/quest/combat/refresh_rate.cpp`, the one owner of the refresh rate (ADR 0039): the
render-configuration handler asks it for the rate `vrapi_SetDisplayRefreshRate` is called with, and a game-thread tick
(`src/quest/combat/combat_tick.h`) looks at the loaded gamespaces' levels every 0.5 s and re-asks through the slot on a change. It runs where the
combat modules run (the combat APKs, `combat::Active()`) and, within them, only on a combat map; anywhere else the game's own rate
stands. The level answer is `src/quest/combat/combat_level.h`; its ids are CSymbol64 values of the names above, checked against
`EchoVR::CSymbol64Hash` by the host test. Three `combat:` keys in `config.yaml`, read once at launch, switch its parts: `refresh_rate: false`
(witness only), `refresh_rate_lobby: true` (the combat lobby too; off by default) and `refresh_rate_quest2_72: false` (a Quest 2 uses 90). The
plugin's kill files `hzlevel.off`, `hzlobby90.off` and `hz72q2.off` are not read; one found is a Warning that names the key above.

## Evidence a run must show

- One `combat_refresh_rate_install` record with `mode` (`match` by default), `quest2`, `level_hz` and the three keys' values.
- A `combat_refresh_rate_switch` record with `sent_hz` `90.0` and `why` `combat or lobby level loaded` (`72.0` on a Quest 2), and the render_config line reporting the same rate, within 1 s of entering a combat map, and `why` `echo arena level loaded: session rate` with `sent_hz` `120.0` within 1 s of loading an Echo Arena level.
  Falsifier: no switch line on a combat map, a switch below 120 in an Echo Arena level, or one in the lobby `mpl_lobby_b2`.

# ADR 0002: the level refresh-rate rule is on by default

Status: accepted (#1737, row G6A-040). Supersedes the default in ADR 0001; ADR 0001's timing analysis stands.

## Context

#1737 asks for every behaviour the q15t source build gives a Quest player. That build's final shipped on-list turns on two
refresh-rate files: `hzlobby90.on` and `hz72q2.on` (the source build's hz module, header comment). Its rule:

- the level rate while a combat map (`mpl_combat_dyson`, `_combustion`, `_fission`, `_gauss`) or a social-lobby level
  (`mpl_lobby_b`, `_b2`, `_b2_lowspec`, `_b_combat`, `_b_arena`, `_b_social`) is loaded;
- the game's own rate (120 on a Quest) while an Echo Arena level (`mpl_arena_a`, `_a_lowspec`, `_b`, `mpl_tutorial_a`) is
  loaded, which wins over a lobby level still loaded during a transition, and in every other state;
- the level rate is 90, or 72 on a Quest 2 (`ro.product.device` is `hollywood`).

ADR 0001 showed the game clock is variable-step, so this is a performance setting, not a timing-parity one.

## Decision

The rule is on by default and lives in `src/quest/combat/refresh_rate.cpp`, the one owner of the refresh rate (ADR 0039): the
render-configuration handler asks it for the rate `vrapi_SetDisplayRefreshRate` is called with, and a game-thread tick
(`src/quest/combat/combat_tick.h`) looks at the loaded gamespaces' levels every 0.5 s and re-asks through the slot on a change. It runs where the
combat modules run (the q15t and combat APKs, `combat::Active()`); anywhere else the game's own rate stands. Level ids are CSymbol64
values of the names above, checked against `EchoVR::CSymbol64Hash` by the host test. Three `combat:` keys in `config.yaml`, read once at
launch, switch its parts: `refresh_rate: false` (witness only), `refresh_rate_lobby: false` (combat maps only) and
`refresh_rate_quest2_72: false` (a Quest 2 uses 90).

## Evidence a run must show

- One `combat_refresh_rate_install` record with `mode` `lobby`, `quest2`, `level_hz` and the three keys' values.
- A `combat_refresh_rate_switch` record with `sent_hz` `90.0` and `why` `combat or lobby level loaded` (`72.0` on a Quest 2), and the render_config line reporting the same rate, within 1 s of entering the lobby or a
  combat map, and `why` `echo arena level loaded: session rate` with `sent_hz` `120.0` within 1 s of loading an Echo Arena level.
  Falsifier: no switch line in the lobby, or a switch below 120 in an Echo Arena level.

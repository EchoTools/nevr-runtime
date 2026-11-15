---
# atwd read contract
read: whole
read_via: cat docs/adr/0039-one-owner-per-game-behaviour.md
audience: all
---

# ADR 0039: each game behaviour this repo changes has one owner, and every other module is an input to it

Status: proposed. Applies to the runtime, the Quest sentinel and every plugin, on both platforms. Builds on AGENTS.md
"Project" (one implementation per game function for PC and Quest) and ADR 0034 (one hook manager per process).

## Context

AGENTS.md "Project" keeps one implementation of a feature across the two binaries. Nothing keeps one owner of a
behaviour within one process.

The sentinel already has the pattern for one behaviour. `src/quest/sentinel/clock_levels_hook.h`: "This module owns the
decision, not the slot." The render configuration hook (`src/quest/sentinel/render_config_hook.cpp`) holds the
`vrapi_SetClockLevels` slot and asks the clock-level module which levels to call the original with. But the combat plugin
changed the clock levels too from its own module (CPU 4 / GPU 4), outside that decision; the clock levels now have the one owner,
`clock_levels` (ADR 0046 rule 3).

The display refresh rate was a different case: the render configuration hook only observed `vrapi_SetDisplayRefreshRate`,
and the combat plugin's rate rule was the one module in this tree that changed it. Its clash with the sentinel was two hook
mechanisms on one slot (ADR 0034, #2000), not two owners. The rule now lives in `src/quest/combat/refresh_rate.h`, which the render
configuration handler asks, as it asks `clock_levels` (ADR 0046 rule 3).

## Decision

1. **One owner per behaviour.** A behaviour is a value the game applies or a function result this repo changes: a clock
   level, a refresh rate, a texture size, a heap size, a provider name. Exactly one module owns its decision.
2. **Every other module is an input.** A module that needs to influence the behaviour gives the owner an input (a
   setting the owner reads, or a request through an interface the owner offers); the owner combines the inputs and
   applies one value. A module may also observe the call through a handler in the manager's chain (ADR 0034) that
   changes neither the arguments nor the result, unless the function is held by a hot-path full replacement (ADR 0038),
   which has no chain: there the observer reads what the replacement reports.
3. **The owner logs the decision**, naming every input it combined (config, plugin request, headset profile) and the
   value applied.
4. **The owner is named where the behaviour is.** The owning file's header says it owns the decision and lists its
   inputs, as `clock_levels_hook.h` does.

## Finding a violation

Scope: `src/quest`, its plugins, `src/runtime`, `src/modules`.

- **O1.** Two modules that each change the same behaviour: the arguments or the result of the same game call, or the
  same game setting. A handler that only observes is not a hit.
- **O2.** A behaviour whose owner's decision line does not name the inputs it combined.

## Consequences

- Clock levels get one decision: the combat plugin's request becomes an input to the clock-level module.
- A config switch and a plugin can no longer fight over a value without either saying so.

## Not claimed

Whether the refresh-rate owner ADR 0046 names (the combat refresh-rate rule, which the render configuration handler asks)
holds on a headset; the run of #2000 shows it. Whether the reference runtime's modules in the release APK's `libvrapi.so`
(kept in the release by `tools/internal/questruntimepin`) change clock levels or the refresh rate: O1 covers this tree's code only.

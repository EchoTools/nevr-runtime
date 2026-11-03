# ADR 0039: each game behaviour this repo changes has one owner, and every other consumer goes through it

Status: proposed. Applies to the runtime, the Quest sentinel and every plugin, on both platforms. Builds on AGENTS.md
"Project" (one implementation per game function for PC and Quest) and ADR 0034 (one hook manager per process).

## Context

AGENTS.md "Project" keeps one implementation of a feature across the two binaries. Nothing keeps one owner of a
behaviour within one process. Today two modules each change the headset's clock levels:

- the sentinel's override, behind `diagnostics.clock_levels_override` (`src/quest/sentinel/clock_levels_hook.cpp`);
- the q15t plugin's CPU 4 / GPU 4 request (`src/quest/plugins/q15t/src/clock_levels.cpp`).

The display refresh rate is also touched twice: the sentinel's render configuration hook on `vrapi_SetDisplayRefreshRate`
(`src/quest/sentinel/render_config_hook.cpp`) and q15t's rate rule (`src/quest/plugins/q15t/src/refresh_rate.cpp`). #2000
is the defect this produces: by code, q15t's two hooks are refused because the sentinel already holds both slots.

## Decision

1. **One owner per behaviour.** A behaviour is a value the game applies or a function result this repo changes: a clock
   level, a refresh rate, a texture size, a heap size, a provider name. Exactly one module owns it.
2. **Others go through the owner.** A second module that needs to influence the behaviour asks the owner: a setting the
   owner reads, or a handler in the owner's chain on the same slot (ADR 0034). It never hooks or writes the same thing
   itself.
3. **The owner logs the decision**, naming every input it combined (config, plugin request, headset profile) and the
   value applied.
4. **An owner is named where the behaviour is.** The owning file's header says it owns the behaviour and lists its
   inputs.

## Finding a violation

Scope: `src/quest`, its plugins, `src/runtime`, `src/modules`.

- **O1.** Two modules that hook the same game function or slot to change its result or arguments (both lists, any file).
- **O2.** Two modules that write the same game setting, global or call parameter.
- **O3.** A behaviour whose decision line does not name the inputs it combined.

## Consequences

- #2000's two behaviours get one owner each, and the plugin's rule becomes an input to it.
- A config switch and a plugin can no longer fight over a value without either saying so.

## Not claimed

Which module should own clock levels and refresh rate; #2000 decides that with its headset run.

# ADR 0037: measurement and debug instrumentation is a build option, off by default, and no release carries it

Status: proposed. Applies to the PC and Quest builds.

## Context

Some questions are answered only by instrumenting the running game: how often a hooked function is called, what size a
read returns, how long an original takes. That code is not meant for players: it adds work on a hot path, logs at a
rate a release does not, and may carry a switch that changes behaviour for an A/B run.

The tree already keeps test-only code out of what ships in three ways:

- `src/runtime/scenario/` (the scenario control endpoint) is compiled in only by the `mingw-scenario` preset and never
  shipped (AGENTS.md, the `src/runtime/` table).
- The unsigned-plugins dev build (`mingw-plugin-dev`, `NEVR_ALLOW_UNSIGNED_PLUGINS`) carries the literal
  `NEVR-PLUGIN-TRUST dev`, and `tools/package_release.py` refuses to package anything but a release (N147).
- The autopilot plugin is carried only in an APK built by its own recipe (`docs/adr/0025-quest-autopilot-route.md`).

## Decision

1. **Instrumentation is a build option.** A probe or debug measurement is compiled in only when a CMake option or preset
   names it; the option defaults to off and no release or package-dev recipe sets it.
2. **A build that carries one says so.** The build's variant name (ADR 0040) names each option it was built with, in
   the identity literal and in the first log line.
3. **The release refuses it.** The release and package-dev recipes refuse an artifact whose identity names a measurement
   option, the way `tools/package_release.py` refuses a non-release build today.
4. **Measuring changes nothing.** Instrumentation calls the original and returns its result. A switch that changes
   behaviour for an A/B run is a named file or setting, read once at start and logged either way, with the consequence.
5. **Its log is bounded.** One install line per instrumented site, then summary lines per fixed window
   (`docs/standards/logging.md`, Rules 5 and 13), never a line per call.

## Finding a violation

Scope: `CMakeLists.txt` files, `CMakePresets.json`, the `justfile`, `tools/`, `src/`.

- **M1.** Instrumentation compiled in by default (no option, or an option that defaults to on).
- **M2.** A release or package-dev recipe that sets a measurement option, or does not refuse an artifact that carries one.
- **M3.** A measurement path that changes a value the game sees without a named, logged switch.
- **M4.** A measurement log line written per call.

## Consequences

- A measurement build can be handed to a tester without risk of reaching players, and its log says what it is.

## Not claimed

That instrumentation costs nothing: a probe's own cost is part of what it measures, and its report says so.

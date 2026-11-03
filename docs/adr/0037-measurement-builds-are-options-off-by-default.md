# ADR 0037: measurement instrumentation is gated by the build, and no release carries it

Status: proposed. Applies to the PC and Quest builds.

## Context

Some questions are answered only by instrumenting the running game: how often a hooked function is called, what size a
read returns, how long an original takes. Instrumentation added for such a question is not meant for players: it adds
work on a hot path, logs at a rate a release does not, and may carry a switch that changes behaviour for an A/B run.

The tree already gates this by the kind of build:

- `nevr_build::kIsDev` is true for every build but a CI build of a release tag (`cmake/nevr_build_info.h.in`). The Quest
  headset probe runs only when it is (`options.probe_enabled = nevr_build::kIsDev` in `src/quest/sentinel/entry.cpp`),
  and ADR 0018's per-request log lines are development builds only.
- `src/runtime/scenario/` is compiled in only by the `mingw-scenario` preset and never shipped (AGENTS.md, the
  `src/runtime/` table).
- The unsigned-plugins dev build (`mingw-plugin-dev`) carries the literal `NEVR-PLUGIN-TRUST dev`, and
  `tools/package_release.py` refuses to package anything but a release (N147).
- The autopilot plugin is carried only in an APK built by its own recipe (`docs/adr/0025-quest-autopilot-route.md`).

Some observation is in every build on purpose, cheap and silent unless a switch turns it on: the PC frame-time copy is
"installed on every client whatever the switch says" and keeps what it reads only while `diagnostics.frame_stats` is on
(`src/runtime/frame/frame_stats.cpp`), and hooks keep plain call counters that their existing reports read
(`src/quest/sentinel/clock_levels_hook.cpp`).

## Decision

1. **Measurement instrumentation is gated by the build.** Per-call timing, hooks installed only to observe, switches
   that change behaviour for a comparison, and per-call or per-request log lines are compiled in or turned on only in a
   development build (`nevr_build::kIsDev`) or behind a CMake option or preset that defaults to off. A release carries
   none of it.
2. **Allowed in every build:** an install whose cost is fixed and small and whose output stays off unless a config switch
   turns it on, and plain counters that an existing per-hook report reads.
3. **A build that carries an option says so.** Its variant name (ADR 0040) names each measurement option it was built
   with, in its first log line.
4. **The release refuses it.** The release recipe refuses an artifact whose variant names a measurement option; a release
   is never a development build by construction.
5. **Measuring changes nothing.** Instrumentation calls the original and returns its result. A switch that changes
   behaviour for an A/B run is a named file or setting, read once at start and logged either way, with the consequence.
6. **Its log is bounded.** One install line per instrumented site, then summary lines per fixed window
   (`docs/standards/logging.md`, Rules 5 and 13).

## Finding a violation

Scope: `CMakeLists.txt` files, `CMakePresets.json`, the `justfile`, `tools/`, `src/`.

- **M1.** Measurement instrumentation (rule 1's kinds) that a release build compiles in and runs: not gated by
  `nevr_build::kIsDev` or by an option that defaults to off.
- **M2.** A release recipe that accepts an artifact whose variant names a measurement option.
- **M3.** A measurement path that changes a value the game sees without a named, logged switch.
- **M4.** A measurement log line written per call in a build a release could ship.

## Consequences

- A measurement build can go to a tester without risk of reaching players, and its log says what it is.

## Not claimed

That instrumentation costs nothing: a probe's own cost is part of what it measures, and its report says so.

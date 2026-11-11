---
# atwd read contract
read: whole
read_via: cat docs/adr/0037-measurement-instrumentation-is-gated-by-the-build.md
audience: all
---

# ADR 0037: measurement instrumentation is gated by the build, and no release carries it

Status: proposed. Applies to the PC and Quest builds.

## Context

Some questions are answered only by instrumenting the running game: how often a hooked function is called, what size a
read returns, how long an original takes. Instrumentation added for such a question is not meant for players: it adds
work on a hot path, logs at a rate a release does not, and is left off once the question is answered.

The tree already gates this kind of code by the build:

- On Quest, `nevr_build::kIsDev` is true for every build but a CI build of a release tag (`cmake/nevr_build_info.h.in`,
  generated for the Quest build only by `cmake/nevr_build_info.cmake`). The headset probe runs only when it is
  (`options.probe_enabled = nevr_build::kIsDev` in `src/quest/sentinel/entry.cpp`), and ADR 0018's per-request log lines
  are development builds only.
- On PC there is no such constant. `src/runtime/scenario/` is compiled in only by the `mingw-scenario` preset and never
  shipped (AGENTS.md, the `src/runtime/` table), and the unsigned-plugins build (`mingw-plugin-dev`) carries the literal
  `NEVR-PLUGIN-TRUST dev`, which `tools/package_release.py` refuses to package (N147).
- The autopilot plugin is carried only in an APK built by its own recipe (`docs/adr/0025-quest-autopilot-route.md`).

Other code looks like measurement but is part of the product, in every build on purpose:

- Records an ADR or the logging standard requires: ADR 0022's render configuration hooks (on Quest a
  "development-free, always-on module", `docs/adr/0022-render-config-log.md`), and `docs/standards/logging.md` Rule 16's
  lobby records.
- Installs whose cost is fixed and small and whose output stays off unless a config switch turns it on: the PC frame-time
  copy is "installed on every client whatever the switch says" and keeps what it reads only while
  `diagnostics.frame_stats` is on (`src/runtime/frame/frame_stats.cpp`).
- Plain counters that a hook's existing report reads (`src/quest/sentinel/clock_levels_hook.cpp`).
- A named setting, off by default, that changes behaviour so a headset A/B can compare: `diagnostics.clock_levels_override`
  "stays off until a headset A/B shows a benefit" (`src/quest/sentinel/quest_config.h`).
- A diagnostic an accepted ADR designs, off by default, that installs nothing while it is off: ADR 0006's hardware dump
  (`diagnostics.hardware_dump`), whose 18 hooks are installed only when the switch is on
  (`docs/adr/0006-quest-hardware-dump.md`).

## Decision

1. **Measurement instrumentation is code whose purpose is to answer a developer's question about the running game**:
   per-call timing, a hook installed only to count or time calls for a measurement, and per-call or per-request log lines
   added for one. It is compiled in or turned on only in a development build or behind a CMake option or preset that
   defaults to off: on Quest, `nevr_build::kIsDev` or such an option; on PC, such an option or preset (PC has no
   build-kind constant). A release build carries none of it.
2. **A release build** is an artifact the release recipes build for a tag (`just release-quest-apk`,
   `release-quest-variant-apk` and `release-quest-combat-apk`, through their shared body `_release-quest-apk`) or that
   `build.yml` builds from a release tag, as `cmake/nevr_build_info.cmake` reads a release tag (an rc tag included).
   Every other build is a development build.
3. **Part of the product, allowed in every build:** the five kinds in Context's second list. A comparison setting follows
   rule 6.
4. **A build that carries an option says so.** Its variant name (ADR 0040) names each measurement option it was built
   with, in its first log line.
5. **The release refuses it.** The release recipe refuses an artifact whose variant names a measurement option.
6. **Measuring changes nothing the game sees, except through a named setting.** Instrumentation calls the original and
   returns its result. A setting that changes behaviour for a comparison is named, off by default, read once at start,
   and logged either way with its consequence, in its own line or in the values it changes (for
   `diagnostics.clock_levels_override`, the render configuration line's `cpu_level` and `gpu_level`).
7. **Its log is bounded in a release build**: one install line per instrumented site, then summary lines per fixed window
   (`docs/standards/logging.md`, Rules 5 and 13). Per-call or per-request lines are allowed only where rule 1 allows the
   instrumentation itself, as ADR 0018's development-build request lines are.

## Finding a violation

Scope: `CMakeLists.txt` files, `CMakePresets.json`, the `justfile`, `tools/`, `src/`. The product kinds of rule 3 are
not hits.

- **M1.** Measurement instrumentation (rule 1) that a release build compiles in and runs.
- **M2.** A release recipe that accepts an artifact whose variant names a measurement option.
- **M3.** Instrumentation that changes a value the game sees other than through a setting that follows rule 6.
- **M4.** A per-call measurement log line that a release build writes.

## Consequences

- A measurement build can go to a tester without risk of reaching players, and its log says what it is.

## Not claimed

That instrumentation costs nothing: a probe's own cost is part of what it measures, and its report says so.

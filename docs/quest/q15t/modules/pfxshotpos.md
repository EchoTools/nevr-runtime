# Module `pfxshotpos` (on-list switch `pfxshotpos`, issue #1634)

Files: `src/quest/plugins/q15t/src/pfxshotpos.{h,cpp}`, `src/quest/plugins/q15t/src/pfxshotpos_core.h`,
`src/quest/tests/q15t/pfxshotpos_test.cpp`, `src/quest/tests/q15t/mod/pfxshotpos.sh`.

## Status

Partial. The decisions are written and host-tested. The install step is not: the probe's hooks sit on the particle-effect
renderer (switch `pfx`, issue #1631), which this build does not carry. With the module on, the install logs one WARNING
(`reason=pfx_renderer_absent`) and installs nothing. This is intended until #1631 lands.

## What the probe is for

Log only. It records where the bullet effects (muzzle, trail and impact nodes, the 20 ids in `pfxshotpos_core.h`) are created,
switched on and re-placed, against the view-0 eye, and whether a ribbon's head jumps. It writes no node, system, cb or position.

## Wiring

- Option `Q15T_PFXSHOTPOS`, default ON. Kill file `pfxshotpos.off` in the plugin data directory (module_switch.h).
- `plugin.cpp` calls `InstallPfxShotPos()` after the decals block and lists `pfxshotpos` in the build banner.

## Decisions in the core (`pfxshotpos_core.h`)

- `ClassOf(sym)`: muzzle, trail, impact, or -1 for ids the probe does not follow.
- `Bin(v, edges, n)`: count of edges at or below `v`; NaN lands in the last bin.
- `Take(count)`: the first 48 lines of a kind, then every 2^k-th.
- `IsJump(prev, cur)`: world position moved more than 2 m between consecutive sim frames; NaN is not a jump.
- `SumDue(now, last)`: one summary line per 10 s.

## Headset test (once #1631 is in the build)

1. Boot with the module on; the log has `pfxshotpos: ...` and no `not installed` line.
2. Fire a shot; the log has create and on lines for the muzzle and trail ids (the line formats land with the hooks).
3. Drop `pfxshotpos.off` into the data directory; the next boot logs `pfxshotpos: off reason=kill_file`.

## Not observed

Nothing on a headset. The install step has no renderer to attach to, so no probe line has been produced by a real run.

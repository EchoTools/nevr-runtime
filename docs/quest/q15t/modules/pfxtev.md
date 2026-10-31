# pfxtev (issue #1642)

The event-effect decision of the q15t particle path. Source: `src/quest/plugins/q15t/src/pfxtev.{h,cpp}`.

## What it decides

Six effect ids (`kPfxTevIds`) are GPU events: an effect on an events graph that spawns child effects. `PfxTevAllows(id)`
answers whether the renderer may create one of them:

- an id that is not one of the six: allowed (this switch does not gate it);
- one of the six while the switch is on: allowed, one INFO line `pfxtev: event-effect create decision` with `id`, `switch`, `result`;
- one of the six while the kill file `pfxtev.off` is in the q15t data directory: refused, the same line with `result=refused`.

The switch is on by default. `InitPfxTev()` reads the kill file once and logs one INFO line for the state. Past 64 decision
lines per process, one WARNING is written and the rest are counted only (`GetPfxTevStats()`).

## Not yet wired (#1631)

Nothing calls `PfxTevAllows` on this tree. The renderer that creates effects is pfx (issue #1631, the parity table row
`pfx`), which is not in q15t. Until #1631 lands, the switch has no effect on a headset.

## Source

The id list and the switch's place in the renderer come from the upstream q15t reference, read as text
(`runtime/native/afkxr/afk_pfx.cpp`, the event tier of `kTierIds`; the kill file there is `pfxtev.on`, a test-only switch that the
working build ships on). Nothing is copied from it: the ids are the six symbol hashes, the decision is written here.

## Not observed on a headset

- That the six ids are the ids the pack's event effects use (not checked against the pack on this tree).
- That the renderer, once #1631 wires the call, creates these effects with the switch on and draws their children.
- Prediction, written before the run: with pfx on and `pfxtev.off` absent, the six events are created and one decision line per
  create is in the log; with `pfxtev.off` placed, the same six creates are refused and their children do not appear.

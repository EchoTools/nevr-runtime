# ADR 0001: 90 Hz in matches is a performance candidate, not timing parity

Status: accepted for the plugin's experiment plan (2026-10-28); its default is superseded by ADR 0002 (the level rule is the
plugin default). The timing analysis below stands. Not observed on a headset in plugin shape.

## Question

The source build (q15t) runs combat matches at 90 Hz and every other place at the game's own rate (120 Hz on a Quest).
A hypothesis: 90 Hz exists so that match timing matches the Rift CV1 (90 Hz), which would make it a correctness or parity
setting the plugin must always apply, not a performance setting to A/B.

Rule set before looking: if the source build states a parity reason, or the game ties match simulation, physics or netcode
timing to the display rate, then 90 Hz is plugin behaviour. Otherwise it is an A/B candidate.

## Evidence

The source build checkout. the disassembly (libr15, build id b243509c…, VA = ELF + 0x100000).

1. **The stated reason is performance.** `the source build's hz:1-3`: the game asks for 120 Hz; "The lobby (App
   2-4 ms) holds 118-120, a match (App 6-12 ms) misses the 8.3 ms budget and sits at 75-110". `:14-16`: `hzmatch90.on`
   exists because "the arena is CPU-bound on Echo's main thread at ~10.7 ms per frame, which fits 90 Hz and not 120".
   `the source build's perflvl_tail:3-4`: "the combat match is CPU / main-thread bound (~12 ms frame…)".
   No file in the tree gives CV1 or Rift parity as a reason: `grep -rIn -w CV1` over `*.md *.cpp *.h` has 0 hits;
   `grep -rIn -i parity` hits only GPU sort ping-pong, particle double-buffer "frame parity" and a visual "PC-parity class"
   in archived `tools/` copies of the particle code, none about refresh rate or the clock.
2. **The default is 120.** the source build's hz:3-5: "Default (user 2026-10-17 'go back to 120 Hz', planner): Echo's own request
   passes unchanged, i.e. 120"; 90 in matches is an opt-in file (`hzmatch90.on`, `:13-19`). A parity requirement would not be
   an opt-in.
3. **The source build asked the parity question and measured it.** the source build's hz:11: `hz72.on` exists "for a frame-locked
   game logic check, planner 10-16". `the source build's pfx:9856-9860`: the RE-47 game-clock witness ("is any
   game logic frame-locked at 90 Hz?"): R ≈ 1.00 means the game clock runs at wall speed; 1.25 at 90 Hz would mean a fixed
   1/72 step per frame. Result, the source build's tools: "RESULT 2026-10-16 … 90 Hz … RE47_CLK game / wall =
   1.000, mean dt 11.16 ms = 1/90". The game clock is not frame-locked.
4. **The engine's game clock is variable-step and follows the display.** disassembly `CR15Game::UpdateClock` (VA 0x12fd724,
   248 bytes): in VR it sets the clock's raw delta `[clock+0x90] = CVR::DeltaTime() * 1e6` every frame. `CVR::UpdateTracking`
   (VA 0x1b71b60): `DeltaTime = now_predicted_display_time - previous`, floored at a minimum (`DAT_038a3438`); the first frame
   uses `1 / <system property 4>` (the display rate). `AGE_X2_READ.txt:27-31` reads the same chain from the binary
   (CEngine::Simulation takes `[CClock+0x3c]`, the clock delta). So simulation advances by real elapsed display time at any
   refresh rate: 120 Hz gives 8.3 ms steps, 90 Hz gives 11.1 ms steps, and game time tracks wall time in both.

Not examined: whether netcode send/receive tick counts are per-frame (a per-frame tick would send 33% more packets at
120 Hz, which is a load difference, not a timing-parity one); whether any physics integration is step-size sensitive enough
to change outcomes between 8.3 ms and 11.1 ms steps. Neither is stated as a reason anywhere in the tree.

## Decision

No timing-parity reason exists in the source build, and the binary's clock is variable-step. **90 Hz in matches is an A/B
performance candidate.** The plugin's default stays the game's own rate. The Hz arm implements the source build's
`hzmatch90` behaviour (90 Hz while a combat map is loaded, the game's own rate elsewhere), level detection through a
game-thread tick on `CncaGame::ExclusiveUpdateGame`'s JUMP_SLOT (a data-page hook), and is graded X vs Hz.

## Prediction for the Hz arm (written before any headset run)

- The sentinel log shows `refresh switch … sent_hz=90.0 why=combat map loaded` within 1 s of a combat map loading, and
  `sent_hz=120.0 why=combat map gone` within 1 s of leaving it. Falsifier: no switch line in a combat match (level detection
  or the tick is wrong: read the `game tick` ERROR line and the install lines).
- OVR Metrics shows the display at 90 Hz during the match and 120 Hz in menus and the lobby. Falsifier: 120 Hz in a match
  after the switch line (the runtime ignored the re-ask).
- Stale frames per second during a combat match are lower in Hz than in X (X runs the match at the game's 120 Hz target with a
  ~10.7 ms CPU frame, so it misses the 8.3 ms budget on most frames). Falsifier: the Hz arm's match stale rate within ±20% of X's.
- Gameplay timing is unchanged in kind (the clock is variable-step): no change in match length or clock behaviour between
  arms. Falsifier: a match timer or movement speed visibly differs between X and Hz.

## Consequences

If the predictions hold, the Hz behaviour becomes the plugin default by a later decision with the measured numbers, not by
this record. If a netcode or physics step sensitivity is found later, this record is reopened.

# Load timing records

Two Info records in the run log (`logs/nevr-<start>.jsonl`), tag `[NEVR.BOOT]`. The text and the wrapper are one header,
`src/runtime/hook/load_timing_rules.h`, shared by the PC runtime and the Quest build (`src/runtime/tests/load_timing_rules_test.cpp` covers it;
`src/quest/tests/load_timing_rules_test.cpp` compiles it for the Quest host build).

## `level_load`: one per `CncaGameSpace::LoadLevel`

    [NEVR.BOOT] level_load level=<name|unknown> level_hash=0x<hex> ms=<n> start_ts_ms=<epoch ms> first_load=<true|false>

- `ms` is the wall time the call took, `-1` when the clock went backwards (the game's own `Finished loading level ... in N ms` line measures inside it).
- `start_ts_ms` is the Unix time in milliseconds when the call began, `-1` when the clock could not be read.
- `first_load` is true for the first level load since process start, so a cold cache reads differently from per-start scanning.
- `level` is the symbol corpus's name for the level's CSymbol64 `level_hash`, `unknown` when the corpus has none.
- On a client, the detour goes in from the deferred bootstrap (`RunDeferredRuntimeBootstrap`). Whether the client's first `LoadLevel` falls after that call is not verified; if it falls before, `first_load=true` lands on the second load.
- Written after the call returns. A load that never returns (the game ends the process) writes none.
- PC: a detour on `CNCAGAMESPACE_LOAD_LEVEL` (`runtime/hook/load_timing.cpp`). When the join hold's detour is installed on that
  address (armed, or left in place after a failed removal), that detour times the load instead, since two detours cannot share an address.
- Quest: not hooked (KNOWN-WRONG #2729, which tracks the Quest hook). The Quest's hooks are build-time libr15 patch rows (ADR 0021); a row for `kLoadLevelVaddr` needs a
  patch-table change, a rebuilt APK and a device check.

## `boot_timing`: one per process

    [NEVR.BOOT] boot_timing engine_ready_ms=<n> logged_in_ms=<n>

Milliseconds from process creation to the game's `Finished initializing engine` line and to the first net game switch to
LoggedIn. `-1` is a mark that never happened. Written once, at the first logged-in switch.

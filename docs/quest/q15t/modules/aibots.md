# Module aibots: Practice AI bots get a scenario each combat match

Port of `<source build>/the source build's aibots` (header lines 1-27, `source-build aibots_tick` 306-346, request protocol `ReadRequest`/`ParseScenario`).
Not ported: the watcher thread and its vptr scan for the CR15Game (the game tick passes it), `debug=1` dump, `aibots.done`.
Status: built and host-tested (2026-10-28). Not observed on a headset. Needs a word patch: no.

## Row
R40 `CR15NetAIScenarios::EnableScenario`, JUMP_SLOT 0x36ce118: called, not hooked, at `base + 0x123c2bc`. The tick is `AddGameTick` (R17), not the source's `vrapi_PollEvent` wrapper
(the source build's match:735-742, `source-build aibots_tick()` at :742).

## Address evidence
- `R0_readelf_r.txt`: `00000000036ce118 ... R_AARCH64_JUMP_SLOT 000000000123c2bc _ZN10NRadEngine8NRadGame18CR15NetAIScenarios14EnableScenarioENS1_14EScenarioStateE`;
  `R1_functions.md` R40: "C6 EnableScenario -> 0x133c2bc (136) ... first word ffc300d1 MATCH". The source header says "libr15 0x123c2bc" (ELF vaddr), disassembly 0x133c2bc = 0x123c2bc + 0x100000.
- Disassembly `FN | 133c2bc | EnableScenario | 136`; first instruction `sub sp, sp, #0x30` = bytes `ffc300d1` = word 0xd100c3ff = the source's `kEnableFirstWord` (the source build's aibots:43).
- `ResolveAiBotsFns`: `dlsym` of the exported name must equal `base + 0x123c2bc` (else refusal), and the first word at that address must be 0xd100c3ff (the source's own check). Fixed address when the name does not resolve.
Layout read from the disassembly: `NetGameMT` 0x13027d4 `mov w8,#0x8510; ldr x0,[x0,x8]`; `Scenarios()` 0x138fd0c `ldr x0,[x0,#0x2ab0]`; `ConnectBot` 0x133bf78
`ldr w8,[x19,#0x500]; add w9,w8,#1; str w9,[x19,#0x500]; mov w9,#0x50; madd x0,x8,x9,x19` (the agent count is a **u32** at scenarios+0x500, rows of 0x50; the source read a u16: same value
for any count < 65536); `EnableScenario` reads `ldr x8,[x19,#0x560]` (current scenario) and stores `str w21,[x19,#0x568]`, and its host test is
`ldr x20,[x0,#0x558]; ldr x8,[x20,#0x2da0]; ldr x8,[x8]; tbnz w8,#1; ... and x8,x8,#0x44; cmp x8,#0x40; b.eq` (host path when flags & 2, or (flags & 0x44) == 0x40; otherwise it
builds and sends a message instead of creating the scenario); `UpdateGameExpressionData` 0x13809f0: `ldr w9,[x0]; cmp w9,#0x9; cset w11,eq; strb w11,[x27,#0x8]` with `x27 = net + 0x6340c`
(state 9 = "in game") and byte stores at `[x27,#0xf]` (net+0x6341b, matchiscombat per the source; the string `matchiscombat` exists at 0x32e1ae0, the pairing with that store is not traced).
Unverified: net+8 (back pointer to the game; `UserActor` and `SendLobbyMemberEvent` do read `[net+8]` as the game), and which store of net+0x6341b carries `matchiscombat`.

## Behaviour (source `source-build aibots_tick`)
Looked at 4 Hz (the source: every frame, with raw dereferences; here every read is `SafeRead`, a syscall).
1. In game = net game present, `net+8 == game`, `i32 net == 9`. Not in game: if a call was made log "re-armed"; forget the count.
2. Combat match = u8 net+0x6341b non-zero; scenario not `off`; scenarios object = net+0x2ab0 non-null.
3. Bot count = u32 scenarios+0x500. Unchanged and (zero, or already called for this count): nothing. Changed: remember it and the time (log INFO `aibots bots`; WARNING when > 14 of the 16-row table).
4. Unchanged for `delay` (default 3 s): `EnableScenario(scenarios, scenario)` (default 1 = RandomAttack; 0 patrol, 2 weaponattack, 3 spike, 4 aithrowtest, 5 aionevone, 6 aibdebugarena), once; again if the count later changes and holds.
Default: ON (the source's). **Runs when**: a combat match is in game and bot agents are connected. Bots exist only in Practice AI, so this is the offline-practice condition: arena, lobby and online
matches have a zero agent count and nothing is ever called. Kept as the source has it: there is no separate offline/host gate (EnableScenario itself takes a client path when the server flags are not set).
The record says `host yes|no` (the engine's own test, above) and `combat_map 0|1` (refresh_rate.cpp's `CombatMapLoaded`, the other detector of a combat level) so a headset run can compare them.
`<data dir>/aibots.req` (`scenario=<0-6|name|off> delay=<s>`), re-read when its mtime changes (1 Hz); an unknown scenario keeps the old one with a WARNING. A changed request does not re-enable by itself.
Added for the run: `aibots idle` (INFO, once per match) when 10 s in game have passed with no call, with matchiscombat, scenarios object and agent count.

## Logging
`aibots on|off` (ERROR when libr15 is not loaded or the address does not check), `aibots req`, `aibots bots`, `aibots enable` (fields: why = scenario name, scenario, bots, detail =
`first call | bot count changed | replacing one already set; scenario object set | NOT set (not the host?); host yes|no; combat_map 0|1; delay_ms`), `aibots re-armed`, `aibots agents`, `aibots idle`.

## Evidence
`src/quest/tests/q15t/aibots_test.cpp` (sections 0-11 incl. 3b and 4b; 0 is the compiled-in defaults before any reset): hold and exact timing, zero bots, not in game / not combat / no scenarios / other game, count change resets the hold, 4 Hz look, re-call on a changed count, re-arm on leaving (and on a vanished net game),
request file (off, scenario names/numbers/range, delay, per-change reads, no re-enable), agent warning, host and combat-map record fields, idle line, address and first-word checks, missing library.
`src/quest/tests/q15t/mutants.d/aibots.sh`: 51 mutants, all killed (one, `ai_scenario_range`, by a crash in the test process on an out-of-range name index, not by a failing check). NDK r26d syntax check clean; no `.init_array`.

## Prediction (written before any headset run)
Offline Practice AI: `aibots on`; on entering the match `aibots bots ... bots=N`; 3 s after the count settles `aibots enable why=randomattack scenario=1 bots=N detail=first call; scenario object set; host yes; combat_map 1 ...`;
the bots move (patrol, fire) instead of standing still; one `enable` per match unless the count changes. An arena or online match: no `enable`, at most one `aibots idle`.
Falsifiers: (i) bots stand still and there is an `aibots idle` line: its `matchiscombat 0` means net+0x6341b is not the flag (or not set yet), `scenarios object no` means net+0x2ab0, `bots=0` while bots are on the
field means scenarios+0x500; (ii) `aibots enable` with `scenario object NOT set` or `host no` in offline practice (EnableScenario took the client path); (iii) `combat_map 0` on `enable` in a combat level (the two detectors disagree);
(iv) a crash at EnableScenario; (v) `aibots off` ERROR (address check failed).

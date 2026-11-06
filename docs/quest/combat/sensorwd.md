# Module `sensorwd` (the scanner-sphere watchdog)

Files: `src/quest/combat/sensorwd.{h,cpp}`, `src/quest/combat/sensorwd_targets.h`, `src/quest/tests/sensorwd_test.cpp`, `src/quest/combat/legacy_switch.h`. A normal combat step, in the
`q15t` and `combat` APKs only. Switch in `config.yaml` (docs/adr/0046 rule 4): `combat.sensorwd`, true unless the file says false.

Pinned build: libr15 build id `b243509c08ce677aeb95fa348016949b3fc45230` (the store file; the running process checks the patched copy's id, `LibR15PatchedBuildId`).
ELF vaddr = disassembly VA - 0x100000.

## Provenance

In kotorvr's (Ducky's) q15t-upstream: `runtime/native/afkxr/afk_fx.cpp` at c5a67e8 (the constants `:6115-6120`, the sphere test, the enable, disable and event hooks, the tick `:6204-6250`) at the
same addresses. It came into this tree with the q15t plugin import (#1591) and moved here from `src/quest/plugins/q15t/src/sensorwd.{h,cpp}`. What is ours: the manager install of the three
words (the plugin swapped them itself), the 34 code-word pins, the ring of waiting pings, the sweep, the log budgets, and the scope in this form (upstream gates the same hooks to combat maps, `afk_fx.cpp:127-137`, `kIsoSns`).

## Default

On. `UPSTREAM-DEFAULTS.md`, row `sensorwd`: upstream's shipped build lists `sensorwd.on` (recipe `on_files`) and level-gates it by default (`afk_fx.cpp:129-137`); the plugin's `Q15T_SENSORWD` was on.
**No headset run of ours has shown it acting:** the q15t sessions log its three `word swapped` lines and `sensorwd on` and nothing after, and the reference build's combat match counts
`fxsensor: counts event tp 0 fp 0 sphere en 0 dis 0 wd off 0 back 0` (`run-quest2-sheet8`): no TriggeredPing, no sphere enable, no watchdog action. Its headset row requires the acting lines below.

## Can it act in arena?

Yes, by construction, so it is scoped. Evidence:

- Two of the three words are generic: `CModelCS::OnComponentEnabled` / `OnComponentDisabled` run for every model component of every level, and `SendComponentEventGlobal` is the game's one component event post.
- The sphere is a model record on a player actor that carries the combat ability script `mp_combat_ability_sensor` (`0x0e1975416e8dcf7a` in `symcache_data.cpp`; the script DLL's strings hold
  `delegate_TriggeredPing`, `fx_pinging_started`, `number_of_pings`, `is_ability_enabled`; searched 1,041 of 1,041 binaries of the echovr project for `TriggeredPing`: `echovr.exe`, `libr15.so`,
  that script and its Quest build). The script name says combat, but `mp_combat_ability_repair_matrix` and the other `mp_combat_ability_*` scripts are the game's ability system, and the engine's
  `THREAT_SCANNER` enum string is in `echovr.exe` and `libr15.so` both. Every script library is registered at startup in an arena session (`script_load` for `lib0e1975416e8dcf7a.so` in
  `run-quest2-sheet7`, an arena lobby session). Whether an arena match gives a player the ability, and so a sphere, is **not** shown by the binaries or by any log here.
- kotorvr found the plugin's sphere hooks live in every level, Echo Arena included, and gated them to combat maps (`afk_fx.cpp:127-137`, "ARENA-ISO1 T-143": "sensorwd and olnoteam in a combat map").

So the watchdog could turn off an arena player's scanner sphere that stays up for 6 s (a change to arena), and it would read game memory on every model enable in arena. Here both are closed:
every handler is a pass-through unless the last look found a combat map and no Echo Arena level, and the look is at most 250 ms old (a look older than 1 s is not believed).

## Arena (same APK)

1. Behaviour. Outside the scope each handler calls the game's function with the same arguments and does nothing else: no read of game memory, no lock, no counter of its own, no line. The tick
   empties the table when the scope ends. `sensorwd_test` `OutsideACombatMapEveryHandlerIsAPassThrough` runs 20 rounds of ping, enable, disable and frame on seven level states (an arena level,
   the low-spec arena level, the tutorial, an arena lobby, the combat lobby, no level, a combat map beside an arena level): the game sees exactly its own 60 calls, every counter is zero, and no
   line is written. `AStaleOrMissingAnswerPassesEverythingThrough` shows no look and a look older than a second pass through. `LeavingTheScopeEmptiesTheTable` shows the table emptied.
2. Cost (ADR 0038), host microbenchmark, x86-64 -O2 (`sensorwd_test` built by hand at -O2; the combat group builds at -O0; best of 7; machine load ~15; NOT a Quest number): on an Echo Arena
   level `OnComponentEnabled` through the thunk costs +3.1 ns over a direct call of 0.3 ns, `SendComponentEventGlobal` +3.4 ns; the tick callback is 3.7 ns a frame between looks, plus a
   real `clock_gettime` of 23 ns, and a frame that looks at the levels (every 250 ms, one gamespace) is 3.3 us. The plugin paid up to five `process_vm_readv` reads on every model enable in every level.
   The calls per frame of the two model functions are NOT measured (no headset run), so ADR 0038's added time per frame is not established; at +3 ns the 10 us line is about 3,000 calls a frame.
   In a combat map the per-call work is the plugin's: up to five reads to ask whether the component is the sphere (not measured on a headset).

## What it is

T-07 `sensorwd.on` (no R-id was assigned in `R1_functions.md`). The threat-scanner sensor sphere is a start-disabled `CModelCR` record
`ac2d3f9f080ba45f` on every player actor; the game's `TriggeredPing` (`delegate_TriggeredPing`, `fd8cf56544b904a3`) enables it and `FinishedPing`
(`delegate_FinishedPing`, `7cc621b8bf3248c5`, +4.0 s) disables it, but on the Quest a sphere stayed drawn for minutes (kotorvr's echoquest-combat W17 log,
kotorvr's echoquest-combat fx:5581-5587, "the wipe band that never ends"). The source build times each sphere enable and turns a sphere that is still on 6 s after its
enable off on the game thread; a sphere it turned off comes back at the next TriggeredPing of its own actor. Only that half is ported. The
`fxsensor.on` log-only probe (event/model lines, 1 s counts thread) is **not built** (see below).

## What it does

State: 32 sphere slots `{cs, h, actor pair, enable time, enabled, forced}` (kotorvr's echoquest-combat fx:5594-5604), up to 8 TriggeredPing actor pairs waiting for the tick,
the last ping `(a, b, time)`. All under one `pthread_mutex_t` (constant-initialised POD, no constructor).

- **Enable wrapper** (`OnComponentEnabled`): if `IsSphere(cs, h)` (five SafeReads, name `== ac2d3f9f080ba45f`): find or take a slot, stamp the
  time, mark enabled, clear `forced`; the slot's actor pair is the last TriggeredPing's if that ping was `<= 250 ms` ago (`kSnsPairMs`,
  kotorvr's echoquest-combat fx:5592) and the slot has no pair yet. Then the stock function runs.
- **Disable wrapper** (`OnComponentDisabled`): a tracked sphere is marked not enabled and not forced. Then the stock function.
- **Event wrapper** (`SendComponentEventGlobal`): `TriggeredPing` records the actor pair as the last ping and queues it for the tick (pairs
  `(0,0)` are not queued); `FinishedPing` is counted. Everything else costs two compares. Then the stock function.
- **Tick** (`SensorWdTick`, game thread, every frame): under the lock, a *forced* sphere whose actor pair is queued is marked enabled with a new
  time (action: "on"); an *enabled* sphere with `now - enable_time >= 6000 ms` (`kWdMs`, kotorvr's echoquest-combat fx:5593) is marked forced (action: "off"); the queue is
  emptied. Outside the lock each action re-checks `IsSphere` and then calls **the stock function itself** (`OnComponentDisabled` / `OnComponentEnabled`
  at `base + 0x15b12a8 / 0x15b127c`, `(cs, h, 0 x6)`), not through the vtable word, so the wrappers do not see the module's own calls
  (kotorvr's echoquest-combat fx:5695-5699). Every 5 s it also re-reads every tracked sphere and drops the ones that no longer read as the sphere.

The component stays "enabled" to the game's script after the tick turned the sphere off, so the script's own next `EnableModel(on)` would be a
no-op; that is why a forced sphere is turned back on at the next TriggeredPing of the actor it was paired with (source comment,
a source-build script docstring).

## Threads and locks

- **Enable / Disable wrappers**: whatever thread the engine calls `EnableModel` on (not known; logged, see below). **Event wrapper**: whatever thread sends
  the event. **Tick**: the game thread (`game_tick.h`: the wrapper on `CncaGame::ExclusiveUpdateGame`, called once per frame by `Render`).
  Each role logs one `sensorwd thread` line (INFO, `role=`, `tid=` from `gettid`, and the last tid seen in each role) the first time it is seen on a thread
  and again when it moves; at most 24 lines per process (then one WARNING "log cap reached").
- One mutex `g_mu`. It is held only around short loops over plain fields (slot table, ping queue, last ping), **never** across a `SafeRead`, a call into
  the game, or a `Log` call, and never nested. The hooks block on it (the critical sections are bounded: 32 slots, 8 pings), they do not `trylock`:
  a dropped state update would lose an enable and leave a sphere unwatched. Tests: the state lock is held at the six sites that take it and
  free after every public call (`TestLockDiscipline`, via the `Hook(11..16)` seams and `SensorWdLockHeldForTest`); no game call and no `Log` call
  happens under it (`g_lock_in_real`, `g_log_under_lock` probes in every single-threaded test); a 4-thread run with a ticker checks the counters add up.
- Counters and the stock-function pointers are atomics. `g_on` gates the wrappers: a rolled-back install leaves inert wrappers that only pass calls on
  (tested: the wrappers captured during a failed install are called afterwards).

## Hooks

Three hooks through the manager, none a code write and none a thunk-less word: `CModelCS::OnComponentEnabled` and `OnComponentDisabled` (ABS64 vtable words `0x36495c8` and `0x36495d0`, each
the only relocation that names its function) and `CncaGameSpace::SendComponentEventGlobal` (JUMP_SLOT `0x36fcae8`). The install is all three or none: the event word first, then enabled, then
disabled, then the tick; a word that does not hold the game's function, or a full tick table, puts back what was done (`ARefusedInstallChangesNothing`). The registers: the functions take integers
in `x0..x4` only (no stack argument, no float, no `x8` result pointer; the bodies end in a tail call), so each thunk is typed with exactly those and hands the result back whole.

## Lines

`combat_sensorwd_install`, `combat_sensorwd_scope` (one per change of the answer), `combat_sensorwd_first` (once per kind: `ping`, `sphere_enable`, `sphere_disable`), `combat_sensorwd_thread`,
`combat_sensorwd_off` and `combat_sensorwd_back_on` (the first 64), `combat_sensorwd_note` (`table_full`, `ping_lost`, `swept`), `combat_sensorwd_cap`, `combat_sensorwd_legacy_switch` (the plugin's
`sensorwd.off` is not read; one found is a Warning). Every handler writes after the game's function has returned.

## Headset row (owed)

On a combat APK built at a tag after this unit, or a development APK repacked with the `combat` variant word, in a combat match with the scanner ability used: `combat_sensorwd_install` with
`"state":"on"` and three `ok` statuses, `combat_sensorwd_scope` `from 0 to 1`, then `combat_sensorwd_first` for `ping` and `sphere_enable`, and `combat_sensorwd_thread` lines naming the roles' threads;
`combat_sensorwd_off` only in a match where a sphere sticks. In an arena match on the same APK: none of these after the install line. Without a scanner used in the match the row is not met.

## Evidence for the behaviour and the constants (kotorvr's echoquest-combat)

| constant / rule | value | source |
|---|---|---|
| sphere record, TriggeredPing, FinishedPing | `ac2d3f9f080ba45f`, `fd8cf56544b904a3`, `7cc621b8bf3248c5` | kotorvr's echoquest-combat fx:5588-5589; the two event ids are `CSymbol64Hash("delegate_TriggeredPing")` / `("delegate_FinishedPing")` (checked in `TestConstants`) |
| stuck after | 6000 ms (FinishedPing comes at 4.0 s) | kotorvr's echoquest-combat fx:5593 |
| pairing window | 250 ms | kotorvr's echoquest-combat fx:5592 |
| slots | 32 | kotorvr's echoquest-combat fx:5594 |
| entry layout | `[cs+0xd0]`, `4*(h&0xffff)`, `[cs+0x100]`, `16*idx`, `+8` model, `+4` visible, `[model+0xcc8]` name | kotorvr's echoquest-combat fx:5586-5587, `5610-5620`; disassembly for all but `+0xcc8` (above) |
| watchdog logic | forced / back-on / off | kotorvr's echoquest-combat fx:5672-5710 |
| default | ON | `sensorwd.on` in the shipped on-list (kotorvr's echoquest-combat on-list) |

## Code-word pins (34, all read in the disassembly)

`kWords` in `src/quest/combat/sensorwd_targets.h`: 11 words of `OnComponentEnabled` (`0x15b127c..0x15b12a4`), 11 of `OnComponentDisabled`
(`0x15b12a8..0x15b12d0`), the first 12 of `SendComponentEventGlobal` (`0x180735c..0x1807388`). The source pins none; these are an addition, the code the
wrappers' reads and argument handling were derived from. Checked with `CheckWordPins` (SafeRead at `base + vaddr`) before any word is swapped. A
pin that fails refuses the module with one ERROR line `sensorwd refused: code word pin mismatch, nothing changed pin=0x<ELF vaddr> found=0x<word or
unreadable> expected=0x<word>`. Verification against the disassembly: a query whose VALUES are generated from the header table (a source-build scratch query, not in this tree):

```
verdict | count
MATCH | 34
```
(no `NOT_SHOWN`, no `MISMATCH`). The words that are `bl`/`br`-relative (`0x180737c`) are position independent in the pinned binary.

After the pins, the three words are read: each must equal `base + its stock function` (`vtable_word_not_stock` / `event_slot_not_stock`, ERROR with
`pin=<slot ELF vaddr> found=<word> expected=<base + fn>`). Nothing is written until all three read right. A swap that fails after another succeeded
(a word changed in between, a page that became executable) puts the earlier ones back (WARNING per word; ERROR if it cannot) and leaves the module off.

## Falsifiers

- `combat_sensorwd_install` with `"state":"refused"`: `pin_mismatch` means the code at a pinned address is not what the handlers were derived from; `hook` names the word that was not the game's function. Nothing is changed.
- Never a `combat_sensorwd_off` line while a sphere stays drawn for minutes: the stuck sphere is not the record `ac2d3f9f080ba45f` (`[model+0xcc8]` is wrong), or its enable does not go through `OnComponentEnabled`.
  If no `enable` or `disable` role appears in the `combat_sensorwd_thread` lines, the vtable words are not what the game calls.
- A `combat_sensorwd_off` line and the sphere stays drawn: the stock `OnComponentDisabled` is not what turns the draw off. A `combat_sensorwd_off` line on every scan pulse right at 6000 ms while the sphere looked normal: FinishedPing's disable is not reaching the handler in time.
- A crash in the model functions or the event path: the integers-only register assumption is wrong, or a freed component was called through the window between the `IsSphere` re-check and the call.
- `combat_sensorwd_scope` never `to 1` in a combat match: the level scan finds no combat map (the pinned table offsets, `combat_level.h`).

## Not verified

The sphere's record id and `[model+0xcc8]`, that model slot `+0x60` is `SetVisible`, the meaning of the event's actor words beyond "x2 and x3, stored together", that TriggeredPing's pair equals the pair of the sphere's actor,
which thread calls `EnableModel` and sends the event, the call rate of the model functions, and every full-match behaviour (no headset run). Whether an arena player can have the sphere is not shown (above).

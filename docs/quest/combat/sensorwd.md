# Module `sensorwd` (the scanner-sphere watchdog)

Files: `src/quest/combat/sensorwd.{h,cpp}`, `src/quest/combat/sensorwd_targets.h`, `src/quest/tests/sensorwd_test.cpp`, `src/quest/combat/legacy_switch.h`. A normal combat step, in the
combat APKs only. Switch in `config.yaml` (docs/adr/0046 rule 4): `combat.sensorwd`, true unless the file says false.

Pinned build: libr15 build id `b243509c08ce677aeb95fa348016949b3fc45230` (the store file; the running process checks the patched copy's id, `LibR15PatchedBuildId`).
ELF vaddr = disassembly VA - 0x100000.

## Provenance

Every line cited here is kotorvr's (Ducky's) echoquest-combat at `c5a67e8`, `runtime/native/afkxr/afk_fx.cpp` (read with `git -C <echoquest-combat> show c5a67e8:runtime/native/afkxr/afk_fx.cpp`):
the comment that names the sphere, the pings and the vtable words `:6108-6114`, the constants `:6115-6121`, the slot `:6125-6130`, the sphere test `SnsIsSphere` `:6141-6152`, the
enable and disable hooks `:6162-6201`, the tick `SnsTick` `:6204-6243`, the event hook from `:6244`, and the gate to combat maps (below). It came into this tree with the combat plugin import
(#1591). What is ours: the manager install of the three words (the plugin swapped them itself), the 34 code-word pins,
the ring of waiting pings, the sweep, the log budgets, and the scope in this form.

The gate to combat maps is two places in upstream: the policy for five features of the file, sensorwd among them, at `:127-137` (the `kIsoSns` gate at `:136`), and `SnsOpen` at
`:6137-6140`, which applies it to the sensor hooks (the enable, disable and event hooks call the game's function and nothing else outside a combat map; the tick does nothing).
Upstream's level answer passes when the level is unknown (`IsoCombat`, `:151`: "unknown: true (the old behaviour)"); ours shuts (no look, or a look older than 1 s, is out of scope).

## Default

On, as the combat plugin's sensorwd option was by default in v6.16.5 (`git show v6.16.5:src/quest/plugins/q15t/CMakeLists.txt`, line 50, default ON).
Upstream the watchdog is opt-in: a `sensorwd.on` file turns it on (`afk_fx.cpp:7911-7916`), and it is gated to combat maps (`afk_fx.cpp:127-137`); whether a shipped upstream build carried that file is not shown here.
**No headset run of ours has shown it acting:** the sessions that loaded the combat plugin log its three `word swapped` lines and `sensorwd on` and nothing after, and the reference build's combat match counts
`fxsensor: counts event tp 0 fp 0 sphere en 0 dis 0 wd off 0 back 0` (`run-quest2-sheet8`): no TriggeredPing, no sphere enable, no watchdog action. Its headset row requires the acting lines below.

## Can it act in arena?

Yes, by construction, so it is scoped. Evidence:

- Two of the three words are generic: `CModelCS::OnComponentEnabled` / `OnComponentDisabled` run for every model component of every level, and `SendComponentEventGlobal` is the game's one component event post.
- The sphere is a model record on a player actor that carries the combat ability script `mp_combat_ability_sensor` (`0x0e1975416e8dcf7a` in `symcache_data.cpp`; the script DLL's strings hold
  `delegate_TriggeredPing`, `fx_pinging_started`, `number_of_pings`, `is_ability_enabled`; searched 1,041 of 1,041 binaries of the echovr project for `TriggeredPing`: `echovr.exe`, `libr15.so`,
  that script and its Quest build). The script name says combat, but `mp_combat_ability_repair_matrix` and the other `mp_combat_ability_*` scripts are the game's ability system, and the engine's
  `THREAT_SCANNER` enum string is in `echovr.exe` and `libr15.so` both. Every script library is registered at startup in an arena session (`script_load` for `lib0e1975416e8dcf7a.so` in
  `run-quest2-sheet7`, an arena lobby session). Whether an arena match gives a player the ability, and so a sphere, is **not** shown by the binaries or by any log here.
- kotorvr found the sphere hooks live in every level, Echo Arena included, and gated them to combat maps (`afk_fx.cpp:127-137`, "ARENA-ISO1 T-143": "sensorwd and olnoteam in a combat map"; applied by `SnsOpen`, `:6137-6140`).

So the watchdog could turn off an arena player's scanner sphere that stays up for 6 s (a change to arena), and it would read game memory on every model enable in arena. Here both are closed:
every handler is a pass-through unless the combat level answer (`combat_scope.h`, `InCombatMap`) says a combat map is loaded and no Echo Arena level is. That answer is the scope row's look
at the loaded levels, four times a second from the game-thread tick, and a look older than 1 s is not believed. The module is one of the scope row's readers (`combat.sensorwd` is in
`ScopeNormal()` in `combat_install.cpp`), and its install refuses with `scope_not_live` when the answer cannot change (the tick or the scope row's refresh is not installed): its handlers
would never act.

## Arena (same APK)

1. Behaviour. Outside the scope each handler calls the game's function with the same arguments and does nothing else: no read of game memory, no lock, no counter of its own, no line. The tick
   empties the table when the scope ends. `sensorwd_test` `OutsideACombatMapEveryHandlerIsAPassThrough` runs 20 rounds of ping, enable, disable and frame on seven level states (an arena level,
   the low-spec arena level, the tutorial, an arena lobby, the combat lobby, no level, a combat map beside an arena level): the game sees exactly its own 60 calls, every counter is zero, and no
   line is written. `AStaleOrMissingAnswerPassesEverythingThrough` shows no look and a look older than a second pass through, and `TheTickDoesNotBelieveAStaleAnswer` shows the tick does not either. `LeavingTheScopeEmptiesTheTable` shows the table emptied. The test drives the real combat level answer: each frame runs the scope row's refresh against the fake game before the module's callback, as the tick does. `OutsideACombatMapEveryHandlerIsAPassThrough` also counts what the three handlers ask: zero reads (`safe_read.h`'s `SetReadCounterForTest` counter) and zero `level::ScanCalls()` over the stream (the sphere check is five reads; `InACombatMapTheSphereCheckReads` shows a combat map does read, so zero is the scope's doing).
2. Cost (ADR 0038), host microbenchmark, x86-64 -O2 (`sensorwd_test` built by hand at -O2; the combat group builds at -O0; best of 7; machine load ~15; NOT a Quest number): on an Echo Arena
   level `OnComponentEnabled` through the thunk costs +3.1 ns over a direct call of 0.3 ns, `SendComponentEventGlobal` +3.4 ns (the handler's arena path is one relaxed load of the scope word
   and a test). A real `clock_gettime` is 23 ns. The module's tick callback adds a clock read, the scope word's load and test and an exchange per frame; the look at the levels (every 250 ms,
   about 3.3 us with one gamespace on the host) is the scope row's, made once for every reader. `sensorwd_test` `TheCostOnAnArenaPath` prints the thunk figures and a frame of the scope's
   refresh and the module's callback together, between looks and with a look; the -O2 figures for the frame are not established. The plugin paid up to five `process_vm_readv` reads on every
   model enable in every level.
   The calls per frame of the two model functions are NOT measured (no headset run), so ADR 0038's added time per frame is not established; at +3 ns the 10 us line is about 3,000 calls a frame.
   In a combat map the per-call work is the plugin's: up to five reads to ask whether the component is the sphere (not measured on a headset).

## What it is

T-07 `sensorwd.on` upstream. The threat-scanner sensor sphere is a start-disabled `CModelCR` record `ac2d3f9f080ba45f` on every player actor; the game's `TriggeredPing`
(`delegate_TriggeredPing`, `fd8cf56544b904a3`) enables it and `FinishedPing` (`delegate_FinishedPing`, `7cc621b8bf3248c5`, +4.0 s) disables it, but on the Quest a sphere stayed drawn
for minutes (upstream's W17 log, `afk_fx.cpp:6110-6111`: "the wipe band that never ends"). The watchdog times each sphere enable and turns a sphere that is still on 6 s after its enable
off on the game thread; a sphere it turned off comes back at the next TriggeredPing of its own actor. Upstream's `fxsensor.on` log-only probe (event and model lines, a 1 s counts line)
is not part of this module.

## What it does

State (`sensorwd.cpp`): `kSlots` (32) sphere slots `{cs, h, actor pair, enable time, enabled, forced}` (upstream's slot, `afk_fx.cpp:6121`, `:6125-6130`), up to `kPings` (8)
TriggeredPing actor pairs waiting for the tick, and the last ping `(a, b, time)`. All under one `pthread_mutex_t` `g_mu` (constant-initialised, no constructor). Every handler's first
statement is `InScope()` (below); outside the scope it calls the game's function and returns.

- **Enable handler** (`OnEnabled`, `OnComponentEnabled`): if `IsSphere(cs, h)` (five `SafeRead`s, the record id at `[model+0xcc8]` equal to `ac2d3f9f080ba45f`): find or take a slot,
  stamp the time, mark it enabled and not forced; the slot takes the last TriggeredPing's actor pair when that ping was at most 250 ms ago (`kPairMs`, upstream `afk_fx.cpp:6119`) and the
  slot has no pair yet. Then the game's function runs, and then the handler writes its lines.
- **Disable handler** (`OnDisabled`, `OnComponentDisabled`): a tracked sphere is marked not enabled and not forced. Then the game's function.
- **Event handler** (`OnEvent`, `SendComponentEventGlobal`): `TriggeredPing` records the actor pair as the last ping and queues it for the tick (a `(0, 0)` pair is not queued; a ping
  with `kPings` already waiting is counted as lost); `FinishedPing` is counted. Any other event is two compares. Then the game's function.
- **Tick** (`Tick` / `OnTick`, registered with the combat tick, `combat_tick.h`, once per frame on the game thread, after the scope row's refresh): it reads the combat level answer
  (`InScope()`, the handlers' test) and keeps the answer it read last, because `combat_scope` has no change callback; a change writes `combat_sensorwd_scope`, and leaving the scope (a combat
  map unloaded, an Echo Arena level loaded, or the answer gone stale) empties the table. In the scope, under the lock, a *forced* sphere whose actor pair is queued is marked enabled
  with a new time (action: back on), an *enabled* sphere with `now - enable time >= kWatchdogMs` (6000 ms, upstream `kWdMs`, `afk_fx.cpp:6120`) is marked forced (action: off), and the
  queue is emptied. Outside the lock each action re-checks `IsSphere` (a sphere that no longer reads as one is dropped and counted stale) and then calls **the game's own function**
  (`OnComponentDisabled` or `OnComponentEnabled`, the thunk's `Original()`), not through the vtable word, so the handlers do not see the module's own calls (upstream does the same,
  `afk_fx.cpp:6228-6232`). Every 5 s (`kSweepMs`) it re-reads every tracked sphere and drops the ones that no longer read as the sphere.

The component stays "enabled" to the game's script after the tick turned the sphere off, so the script's own next `EnableModel(on)` would be a no-op; that is why a forced sphere is
turned back on at the next TriggeredPing of the actor it was paired with (upstream's slot comment, `afk_fx.cpp:6129`).

## Threads and locks

- **Enable, disable and event handlers**: whatever thread the engine calls them on (not known; logged, see below). **Tick**: the game thread (the combat tick, `combat_tick.h`).
  Each role writes one `combat_sensorwd_thread` line (Info, `role`, `tid` from `gettid`, and the last tid seen in each role) the first time it is seen on a thread and again when it
  moves; at most `kThreadLines` (24) a process, then one `combat_sensorwd_cap` Warning.
- One mutex, `g_mu`. It is held only around short loops over plain fields (the slot table, the ping queue, the last ping), never across a `SafeRead`, a call into the game or a log line,
  and never nested. The handlers block on it (the critical sections are bounded: 32 slots, 8 pings); they do not `trylock`, because a dropped update would lose an enable and leave a
  sphere unwatched. Tests (`sensorwd_test`): the lock is held at six of the seven sites that take it (the `SetHookForTest` stages 11 to 16, checked with `LockHeldForTest`; the seventh, `ClearTable`, has no stage and is not checked), the game's
  function runs with the lock free and no line is written under it (each recorded call and line carries `LockHeldForTest()`; `AStuckSphereIsTurnedOffAfterSixSeconds`), and a run on
  several threads with a ticker checks that the counters add up (`TheCountersAddUpUnderThreads`).
- The counters, the tick's last reading of the scope and the hook state are atomics. Outside the scope the handlers do nothing but call the game's function (`InScope()`, one relaxed load
  of the scope word in arena). A refused
  install puts back every word it made (`Undo`) and disarms the thunk, and a disarmed thunk passes every call through (`ARefusedInstallChangesNothing`).

## Hooks

Three hooks through the manager, each a typed thunk and none a code write: `CModelCS::OnComponentEnabled` and `OnComponentDisabled` (ABS64 vtable words `0x36495c8` and `0x36495d0`, each
one of the two ABS64 words in the file that name its function, so each target pins its word) and `CncaGameSpace::SendComponentEventGlobal` (JUMP_SLOT `0x36fcae8`). The install is all three or none: after the build id and the 34 code words, the event word first,
then enabled, then disabled, then the tick; a word that does not hold the game's function, or a full tick table, puts back what was done (`Undo`; `ARefusedInstallChangesNothing`). The registers: the functions take integers
in `x0..x4` only (no stack argument, no float, no `x8` result pointer; the bodies end in a tail call), so each thunk is typed with exactly those and hands the result back whole.

## Lines

`combat_sensorwd_install` (a refusal names its reason, `scope_not_live` among them), `combat_sensorwd_scope` (one per change of the answer as the tick reads it, `from` and `to`; the level is on the
scope row's `combat_scope_change` line of the same frame), `combat_sensorwd_first` (once per kind: `ping`, `sphere_enable`, `sphere_disable`), `combat_sensorwd_thread`,
`combat_sensorwd_off` and `combat_sensorwd_back_on` (the first 64), `combat_sensorwd_note` (`table_full`, `ping_lost`, `swept`), `combat_sensorwd_cap`, `combat_sensorwd_legacy_switch` (the plugin's
`sensorwd.off` is not read; one found is a Warning). Every handler writes after the game's function has returned.

## Headset row (owed)

On a combat APK built at a tag after this unit, or a development APK repacked with the `combat` variant word, in a combat match with the scanner ability used: `combat_sensorwd_install` with
`"state":"on"` and three `ok` statuses, `combat_sensorwd_scope` `from 0 to 1`, then `combat_sensorwd_first` for `ping` and `sphere_enable`, and `combat_sensorwd_thread` lines naming the roles' threads;
`combat_sensorwd_off` only in a match where a sphere sticks. In an arena match on the same APK: none of these after the install line. Without a scanner used in the match the row is not met.

## Evidence for the behaviour and the constants (echoquest-combat `c5a67e8`)

| constant / rule | value | source |
|---|---|---|
| sphere record, TriggeredPing, FinishedPing | `ac2d3f9f080ba45f`, `fd8cf56544b904a3`, `7cc621b8bf3248c5` | `afk_fx.cpp:6115-6116`; the two event ids are `CSymbol64Hash("delegate_TriggeredPing")` / `("delegate_FinishedPing")` (checked in `TheConstantsArePinned`) |
| stuck after | 6000 ms (FinishedPing comes at 4.0 s) | `afk_fx.cpp:6120` |
| pairing window | 250 ms | `afk_fx.cpp:6119` |
| slots | 32 | `afk_fx.cpp:6121` |
| entry layout | `[cs+0xd0]`, `4*(h&0xffff)`, `[cs+0x100]`, `16*idx`, `+8` model, `+4` visible, `[model+0xcc8]` name | `afk_fx.cpp:6113-6114`, `:6141-6152`; disassembly for all but `+0xcc8` (above) |
| watchdog logic | forced / back-on / off | `afk_fx.cpp:6204-6243` (the tick), `:6246-6258` (the ping it waits for) |
| default | ON here (the combat plugin's sensorwd option, default on in v6.16.5); opt-in upstream | `afk_fx.cpp:7911-7916` (the `sensorwd.on` file that turns it on), `:127-137` (the gate) |

## Code-word pins (34, all read in the disassembly)

`kWords` in `src/quest/combat/sensorwd_targets.h`: 11 words of `OnComponentEnabled` (`0x15b127c..0x15b12a4`), 11 of `OnComponentDisabled`
(`0x15b12a8..0x15b12d0`), the first 12 of `SendComponentEventGlobal` (`0x180735c..0x1807388`). The source pins none; these are an addition, the code the
handlers' reads and argument handling were derived from. `FirstPinMismatch` reads each word at `base + vaddr` after the build id is checked and before any
word is touched; a pin that differs refuses the module: one Error `combat_sensorwd_install` with `"state":"refused"`, `"reason":"pin_mismatch"`, and nothing changed. Verification against the disassembly: a query whose VALUES are generated from the header table (a source-build scratch query, not in this tree):

```
verdict | count
MATCH | 34
```
(no `NOT_SHOWN`, no `MISMATCH`). The words that are `bl`/`br`-relative (`0x180737c`) are position independent in the pinned binary.

After the pins, each of the three words is installed by the manager (`InstallThunk`), which takes a word only while it holds the game's function; a word it refuses ends the install
with `"reason":"hook"` and the manager's status for each word (`enabled`, `disabled`, `event_slot`). The words already taken are put back first (`Undo`), so the module is all three
words or none, and a word that cannot be put back keeps a disarmed thunk, which passes every call to the game's function.

## Falsifiers

- `combat_sensorwd_install` with `"state":"refused"`: `pin_mismatch` means the code at a pinned address is not what the handlers were derived from; `hook` names the word that was not the game's function. Nothing is changed.
- Never a `combat_sensorwd_off` line while a sphere stays drawn for minutes: the stuck sphere is not the record `ac2d3f9f080ba45f` (`[model+0xcc8]` is wrong), or its enable does not go through `OnComponentEnabled`.
  If no `enable` or `disable` role appears in the `combat_sensorwd_thread` lines, the vtable words are not what the game calls.
- A `combat_sensorwd_off` line and the sphere stays drawn: the stock `OnComponentDisabled` is not what turns the draw off. A `combat_sensorwd_off` line on every scan pulse right at 6000 ms while the sphere looked normal: FinishedPing's disable is not reaching the handler in time.
- A crash in the model functions or the event path: the integers-only register assumption is wrong, or a freed component was called through the window between the `IsSphere` re-check and the call.
- `combat_sensorwd_scope` never `to 1` in a combat match: the combat level answer finds no combat map (`combat_scope_change` never `map 1`: the pinned table offsets, `combat_level.h`).

## Not verified

The sphere's record id and `[model+0xcc8]`, that model slot `+0x60` is `SetVisible`, the meaning of the event's actor words beyond "x2 and x3, stored together", that TriggeredPing's pair equals the pair of the sphere's actor,
which thread calls `EnableModel` and sends the event, the call rate of the model functions, and every full-match behaviour (no headset run). Whether an arena player can have the sphere is not shown (above).

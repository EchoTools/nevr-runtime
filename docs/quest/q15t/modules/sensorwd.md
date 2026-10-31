# Module `sensorwd` (T-07, the `sensorwd.on` half)

Files: `src/sensorwd.{h,cpp}`, `src/quest/tests/q15t/sensorwd_test.cpp`, `src/quest/tests/q15t/mod/sensorwd.sh`, `src/quest/tests/q15t/mutants.d/sensorwd.sh`.
Pinned build: libr15 build id `b243509c08ce677aeb95fa348016949b3fc45230` (the disassembly). ELF vaddr = disassembly VA - 0x100000.
Source citations are in "the source build" (`the source build's fx`, generator the source build's tools);
Disassembly citations are quoted from read-only queries (scratch queries, a source-build note not in this tree, run with the
contract's recipe).

## For the main seat: wiring

- Option macro `Q15T_SENSORWD`, default **1 (ON)**: the source build ships `sensorwd.on` in its on-list (its commit subject:
  "on-list +drrlow / p18add / sensorwd"; the list itself, the source build's on-list file, ends with `"sensorwd.on"`).
- Install and tick, next to the other modules in `NvrPluginInitEx`:
  ```cpp
  #if Q15T_SENSORWD
    ReportModule("sensorwd", InstallSensorWd(base));
    AddModuleTick("sensorwd", &SensorWdTick, &want_tick);  // one bool test per frame when the install was refused
  #endif
  ```
  plus `#include "sensorwd.h"`, the `"sensorwd "` entry in the module list string, and `src/quest/plugins/q15t/src/sensorwd.cpp` in the plugin sources (CMake).
  `SensorWdTick` is a `TickFn` (game_tick.h); I did not register it. It takes one of the 16 `kMaxGameTicks` slots (`game_tick.h`).
- No capability bit is needed: the module changes what is drawn (a stuck scanner sphere), not a rule (same class as `hudmeter` / `shieldgamma`).
- Kill file: `<plugin data dir>/sensorwd.off` (`/sdcard/Android/data/<pkg>/files/q15t/sensorwd.off`, `module_switch.h`). Present at install: the
  module refuses with reason `off_file` and one INFO line `sensorwd off`; `ReportModule` will show that as a non-ok status. No `setprop` switch.
- The build-id gate is the plugin's InitEx (not repeated here). Gate order inside the module: kill file, 34 code-word pins, the three words
  hold the stock functions, then the swaps. There is no MD5 or other file-hash gate.
- Host test: `Q15T_MODULE=sensorwd bash src/quest/tests/q15t/run_host_tests.sh` (picked up by the `src/quest/tests/q15t/mod/*.sh` loop; also does the NDK arm64 compile and the
  no-`.init_array` check). Mutants: `Q15T_MUTANTS_ONLY=sensorwd bash src/quest/tests/q15t/mutants.sh` (picked up by the `src/quest/tests/q15t/mutants.d/*.sh` loop).

## Row covered

T-07 `sensorwd.on` (no R-id was assigned in `R1_functions.md`). The threat-scanner sensor sphere is a start-disabled `CModelCR` record
`ac2d3f9f080ba45f` on every player actor; the game's `TriggeredPing` (`delegate_TriggeredPing`, `fd8cf56544b904a3`) enables it and `FinishedPing`
(`delegate_FinishedPing`, `7cc621b8bf3248c5`, +4.0 s) disables it, but on the Quest a sphere stayed drawn for minutes (the source build's W17 log,
the source build's fx:5581-5587, "the wipe band that never ends"). The source build times each sphere enable and turns a sphere that is still on 6 s after its
enable off on the game thread; a sphere it turned off comes back at the next TriggeredPing of its own actor. Only that half is ported. The
`fxsensor.on` log-only probe (event/model lines, 1 s counts thread) is **not built** (see below).

## Mechanism: three data words, no code word

All three through `got_swap.h` / `import_hook.h` (`InstallImportHook(..., expected_vaddr)`: the word must hold exactly `base + the stock function`,
data pages only, RELRO restored, no `PROT_EXEC`, no code write).

| word | ELF vaddr | stock target (ELF) | what it is | evidence |
|---|---|---|---|---|
| `CModelCS` vtable word `OnComponentEnabled` | `0x36495c8` | `0x15b127c` | R_AARCH64_ABS64, RELRO | `R0_readelf_r.txt:49336`: `00000000036495c8 ... R_AARCH64_ABS64 00000000015b127c _ZN10NRadEngine8CModelCS18OnComponentEnabledENS_18SCompactPoolHandleE + 0` |
| `CModelCS` vtable word `OnComponentDisabled` | `0x36495d0` | `0x15b12a8` | R_AARCH64_ABS64, RELRO | `R0_readelf_r.txt:49338`: `00000000036495d0 ... R_AARCH64_ABS64 00000000015b12a8 _ZN10NRadEngine8CModelCS19OnComponentDisabledENS_18SCompactPoolHandleE + 0` |
| `CncaGameSpace::SendComponentEventGlobal(CSymbol64, SActor, CSymbol64, unsigned)` JUMP_SLOT | `0x36fcae8` | `0x180735c` (libr15's own) | R_AARCH64_JUMP_SLOT | `R0_readelf_r.txt:137713`: `00000000036fcae8 ... R_AARCH64_JUMP_SLOT 000000000180735c _ZN10NRadEngine13CncaGameSpace24SendComponentEventGlobalENS_9CSymbol64ENS_6SActorES1_j + 0` |

(The source repoints the two vtable words only: `kSnsEnSlot/kSnsEnFn/kSnsDisSlot/kSnsDisFn`, the source build's fx:5590; its event hook is found by name through
`JumpSlot`, the source build's fx:5884-5900, `5782`. Here the event slot is the pinned one above, from `readelf -rW`.) The two vtable words sit in the
class's vtable `_ZTVN10NRadEngine8CModelCSE` (`R0_readelf_r.txt:99007`, GLOB_DAT to `0x3649560`); the disassembly's `functions` table:
`16b127c | OnComponentEnabled | 44 | NRadEngine::CModelCS`, `16b12a8 | OnComponentDisabled | 44 | NRadEngine::CModelCS`,
`190735c | SendComponentEventGlobal | 48 | NRadEngine::CncaGameSpace` (ELF = VA - 0x100000).

Disassembly instructions of the two stock functions (ELF address, bytes as stored, text), the lookup the module repeats:
```
15b127c 086840f9 ldr x8, [x0, #0xd0]            15b12a8 086840f9 ldr x8, [x0, #0xd0]
15b1280 293c4092 and x9, x1, #0xffff            15b12ac 293c4092 and x9, x1, #0xffff
15b1284 29f57ed3 lsl x9, x9, #0x2               15b12b0 29f57ed3 lsl x9, x9, #0x2
15b1288 08696978 ldrh w8, [x8, x9, LSL #0x0]    15b12b4 e1031f2a mov w1, wzr
15b128c 098040f9 ldr x9, [x0, #0x100]           15b12b8 08696978 ldrh w8, [x8, x9, LSL #0x0]
15b1290 2811088b add x8, x9, x8, LSL #0x4       15b12bc 098040f9 ldr x9, [x0, #0x100]
15b1294 000540f9 ldr x0, [x8, #0x8]             15b12c0 2811088b add x8, x9, x8, LSL #0x4
15b1298 010540b9 ldr w1, [x8, #0x4]             15b12c4 000540f9 ldr x0, [x8, #0x8]
15b129c 090040f9 ldr x9, [x0]                   15b12c8 080040f9 ldr x8, [x0]
15b12a0 223140f9 ldr x2, [x9, #0x60]            15b12cc 023140f9 ldr x2, [x8, #0x60]
15b12a4 40001fd6 br x2                          15b12d0 40001fd6 br x2
```
So: `[cs+0xd0]` is a u16 table read at `4 * (h & 0xffff)` giving an entry number, `[cs+0x100]` the entry array (16 bytes each), entry `+8` the
`CModel*`, entry `+4` the visible flag that Enable passes to the model's vtable slot `+0x60` (Disable passes 0); both are tail calls (`br x2`).
This is exactly the source's `SnsIsSphere` layout (the source build's fx:5610-5620: `[cs+0xd0]`, `4*(h&0xffff)`, `[cs+0x100]`, `16*idx+8`, `+4`).
**Verified** here: those offsets and strides. **Unverified**: `[model+0xcc8]` as the record id (source only, the source build's fx:5586, `5615`) and that
`ac2d3f9f080ba45f` is what sits there; the model-slot `+0x60` is the model's SetVisible (the source's comment, a source-build script docstring; not read).

`SendComponentEventGlobal` (first 12 instructions, ELF `0x180735c..0x1807388`):
`sub sp,sp,#0x40; mov x8,#-1; stp x8,x30,[sp,#0x28]; ldr x8,[x0,#0x58]; stp x2,x3,[sp,#0x18]; mov w2,w4; stp x1,x8,[sp,#8]; add x1,sp,#8; bl ...; ldr x30,[sp,#0x30]; add sp,sp,#0x40; ret`:
x1 is the symbol, x2 and x3 are stored side by side (the actor pair the source calls `a`, `b`), w4 is `j`. That matches the source hook's
`(gs, sym, a, b, j, ...)` (the source build's fx:5711). The mangled name reads `(CSymbol64, SActor, CSymbol64, unsigned)` (`S1_` is the first substitution = `CSymbol64`):
a symbol, an 8-byte actor and a second symbol; the module keys on the pair `(a, b)` exactly as the source does and does not interpret it
(that meaning is the mangled name's, **unverified** beyond the register layout). The body uses `x8` only as its own scratch (no result pointer).

## Calling convention: plain C++, no naked thunk

The three wrappers take `x0..x7` as eight integers and call the stock function with the same eight values and return its `x0`. That is the whole
convention for these functions: the disassembly bodies above take integers only (`this` + an 8-byte handle; `this` + symbol + two actor words + `w4`), use
no stack argument, no float register and no `x8` result pointer, and Enable/Disable end in a tail call. This is the source's own form
(the source build's fx:5660-5669, "every hook reads with SafeRead and calls the original with x0..x7 unchanged"), so powerboost's naked thunk is not needed. Test:
`TestPassThrough` calls each wrapper through the swapped word with distinct values in `x2..x7` and checks the stock function received them unchanged
and its return value came back.

## What it does

State: 32 sphere slots `{cs, h, actor pair, enable time, enabled, forced}` (the source build's fx:5594-5604), up to 8 TriggeredPing actor pairs waiting for the tick,
the last ping `(a, b, time)`. All under one `pthread_mutex_t` (constant-initialised POD, no constructor).

- **Enable wrapper** (`OnComponentEnabled`): if `IsSphere(cs, h)` (five SafeReads, name `== ac2d3f9f080ba45f`): find or take a slot, stamp the
  time, mark enabled, clear `forced`; the slot's actor pair is the last TriggeredPing's if that ping was `<= 250 ms` ago (`kSnsPairMs`,
  the source build's fx:5592) and the slot has no pair yet. Then the stock function runs.
- **Disable wrapper** (`OnComponentDisabled`): a tracked sphere is marked not enabled and not forced. Then the stock function.
- **Event wrapper** (`SendComponentEventGlobal`): `TriggeredPing` records the actor pair as the last ping and queues it for the tick (pairs
  `(0,0)` are not queued); `FinishedPing` is counted. Everything else costs two compares. Then the stock function.
- **Tick** (`SensorWdTick`, game thread, every frame): under the lock, a *forced* sphere whose actor pair is queued is marked enabled with a new
  time (action: "on"); an *enabled* sphere with `now - enable_time >= 6000 ms` (`kWdMs`, the source build's fx:5593) is marked forced (action: "off"); the queue is
  emptied. Outside the lock each action re-checks `IsSphere` and then calls **the stock function itself** (`OnComponentDisabled` / `OnComponentEnabled`
  at `base + 0x15b12a8 / 0x15b127c`, `(cs, h, 0 x6)`), not through the vtable word, so the wrappers do not see the module's own calls
  (the source build's fx:5695-5699). Every 5 s it also re-reads every tracked sphere and drops the ones that no longer read as the sphere.

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

## Evidence for the behaviour and the constants (source build)

| constant / rule | value | source |
|---|---|---|
| sphere record, TriggeredPing, FinishedPing | `ac2d3f9f080ba45f`, `fd8cf56544b904a3`, `7cc621b8bf3248c5` | the source build's fx:5588-5589; the two event ids are `CSymbol64Hash("delegate_TriggeredPing")` / `("delegate_FinishedPing")` (checked in `TestConstants`) |
| stuck after | 6000 ms (FinishedPing comes at 4.0 s) | the source build's fx:5593 |
| pairing window | 250 ms | the source build's fx:5592 |
| slots | 32 | the source build's fx:5594 |
| entry layout | `[cs+0xd0]`, `4*(h&0xffff)`, `[cs+0x100]`, `16*idx`, `+8` model, `+4` visible, `[model+0xcc8]` name | the source build's fx:5586-5587, `5610-5620`; disassembly for all but `+0xcc8` (above) |
| watchdog logic | forced / back-on / off | the source build's fx:5672-5710 |
| default | ON | `sensorwd.on` in the shipped on-list (the source build's on-list) |

## Code-word pins (34, all read in the disassembly)

`kSensorWdWordPins` in `src/quest/plugins/q15t/src/sensorwd.h`: 11 words of `OnComponentEnabled` (`0x15b127c..0x15b12a4`), 11 of `OnComponentDisabled`
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

## Logging (AGENTS.md)

All through `Log` (INFO/WARNING/ERROR; no chatter; no secrets):
- install: INFO `sensorwd word swapped hook=... slot_vaddr=... real_vaddr=... status=ok` x3 and `sensorwd on: ... enable_slot=0x36495c8 disable_slot=0x36495d0
  event_slot=0x36fcae8 watchdog_ms=6000 kill_file=sensorwd.off`; refusals: INFO `sensorwd off` (kill file), ERROR `code word pin mismatch` /
  `sensorwd refused: ... is not the stock function` / `sensorwd refused: a word could not be swapped`, WARNING/ERROR for put-backs.
- behaviour changes: INFO `sensorwd off: sphere still enabled 6000 ms after its enable cs=.. h=.. age_ms=.. n=.. t_ms=..` and `sensorwd back on: its actor
  pinged again ...` (64 per process, then one WARNING cap line, as the source's `kSnsLines`); WARNING `no free slot for a sphere enable` (12 per process),
  WARNING `TriggeredPing dropped, 8 are already waiting for the tick`, INFO `tracked spheres whose component no longer reads as the sphere were dropped`.
- the call paths of the three wrappers log nothing per call; the counters (`GetSensorWdStats()`) carry the rest.

A real record from the host test's last run (fake game image, fake clock; ping at t, enable +120 ms, stuck, off by the tick at +6100 ms, the actor
pings again, back on):
```
L1 plugin=q15t sensorwd word swapped hook=CncaGameSpace::SendComponentEventGlobal (JUMP_SLOT) slot_vaddr=0x36fcae8 real_vaddr=0x300200 status=ok
L1 plugin=q15t sensorwd word swapped hook=CModelCS::OnComponentEnabled (vtable word) slot_vaddr=0x36495c8 real_vaddr=0x300000 status=ok
L1 plugin=q15t sensorwd word swapped hook=CModelCS::OnComponentDisabled (vtable word) slot_vaddr=0x36495d0 real_vaddr=0x300100 status=ok
L1 plugin=q15t sensorwd on: a sphere still enabled 6000 ms after its enable is turned off on the game thread enable_slot=0x36495c8 disable_slot=0x36495d0 event_slot=0x36fcae8 watchdog_ms=6000 kill_file=sensorwd.off
L1 plugin=q15t sensorwd off: sphere still enabled 6000 ms after its enable cs=55eaf11f4350 h=abcd00030000 age_ms=6100 n=1 t_ms=106220
L1 plugin=q15t sensorwd back on: its actor pinged again cs=55eaf11f4350 h=abcd00030000 age_ms=0 n=1 t_ms=136220
```

## Differences from the source build

- Not built: `fxsensor.on` (the log-only probe: per-event and per-model lines, the 1 s `fxsensor: counts` thread), `setprop debug.<app>.sensorwd`
  and the `sensorwd.on` file switch (replaced by the build option, default on, and the `sensorwd.off` kill file). No MD5 / file-hash gate.
- The event hook is **required** here (the source treats it as optional): without it a sphere the module turned off would never come back, which is
  worse than the bug. If its slot is not the pinned word the whole module refuses and nothing is swapped.
- Additions, each tested: (1) a forced sphere's actor pair is matched against a queue of up to 8 pings per tick, not one (two actors pinging in one
  frame both come back); (2) the tick re-checks that the component still reads as the sphere before calling into it (a freed or recycled component is
  dropped, never called), and a 5 s sweep drops dead components so the 32 slots cannot fill with ones from earlier matches; (3) a disable of a sphere
  that was never enabled takes no slot; (4) the 34 code-word pins and the slot/word checks before any write; (5) the hooks do nothing and pass through
  while `g_on` is false; (6) every read of game memory is a SafeRead (the source's too) and a failed read calls the stock function untouched.
- Per-call cost: Enable/Disable on any model: five `process_vm_readv` syscalls (the SafeRead chain) before the stock call, and one more thread-id
  syscall only on a sphere. The call rate of `EnableModel` on this build is not measured (**unverified**); the source pays the same reads
  (the source build's fx:5610-5620). The event wrapper adds two compares to every global component event.

## Prediction (written before any headset run)

On the pinned build, module on, in a combat match with the scanner ability: the log shows the three `sensorwd word swapped ... status=ok` lines and
`sensorwd on` at start, then `sensorwd thread role=...` lines (the tick on the game thread's tid; the enable/disable/event tids tell which threads
call what). Each scan pulse is normal (a ping, the sphere on, FinishedPing 4.0 s later, off). In a match where a sphere sticks: within one frame of
6.0 s after its enable a line `sensorwd off: sphere still enabled 6000 ms after its enable ... age_ms=6000..6100` appears and the sphere stops being drawn
(the wipe band ends); at the same actor's next ping a `sensorwd back on` line appears and the next pulse shows normally. `GetSensorWdStats().wd_off` is 0 in a
match where no sphere sticks.

## Falsifiers

- `sensorwd refused: code word pin mismatch` (ERROR, pin/found/expected named) on a build the plugin accepted by build id: the code at that address is not
  what the wrappers were derived from (or the pin table has a typo); nothing is swapped.
- `... is not the stock function` with `status=unexpected_original`: a vtable word or the event slot does not hold `base + the pinned function`.
- Never a `sensorwd off:` line while the sphere stays drawn for minutes: the stuck sphere is not the record `ac2d3f9f080ba45f` (`[model+0xcc8]` is
  wrong), or its enable does not go through `OnComponentEnabled`, or the watched time is wrong. If `thread` lines show the enable/disable roles
  never seen, the vtable words are not what the game calls (e.g. direct calls to the function).
- A `sensorwd off` line and the sphere stays drawn: the stock `OnComponentDisabled` is not what turns the draw off (the source's claim, a source-build script
  docstring: "= SetVisible 0, what FinishedPing's EnableModel(off) reaches"; not read here).
- A `sensorwd off` line on every scan pulse right at 6000 ms while the sphere looked normal: FinishedPing's disable is not reaching the wrapper in time.
- A crash in `OnComponentEnabled/Disabled` or in the event path: the plain-C++ wrapper's register assumption (integers only in `x0..x7`) is wrong for the
  event function, or a freed component was called through the stale-pointer window between the `IsSphere` re-check and the call (cannot be closed from outside).

## Needs a word patch / not built

- **Needs a word patch:** none. Only data words (two vtable words, one JUMP_SLOT) are written.
- Not built: `fxsensor.on` and its 1 s summary thread; the file / property switches; the MD5 gate.

## Unverified

- `[model+0xcc8]` as the model record id and the id value `ac2d3f9f080ba45f` on this build (source only); that model slot `+0x60` is SetVisible.
- The meaning of the event's `a`, `b` beyond "x2 and x3, stored together" (mangled name: an `SActor` and a `CSymbol64`), and that TriggeredPing's
  pair equals the pair of the sphere's actor (the source pairs by enable time within 250 ms of the ping).
- Which thread calls `EnableModel` / sends the event (the `sensorwd thread` lines will say); whether the game thread is the only one that runs the tick
  is `game_tick.h`'s claim, not re-read here.
- The call rate of Enable/Disable (cost of five reads per call); the full-match behaviour (no headset run).
- On-device run of the source's `sensorwd.on` (the source commit is a build recipe; the generator's docstring describes the W17 evidence, not read).
- The wrappers have never run on arm64: the host tests run them on x86 against a fake image, the NDK arm64 compile only proves it compiles with no
  static initializer.

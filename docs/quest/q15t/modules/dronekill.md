# Module `dronekill` (rows R45, R47, R48, and the Actor slot Enter's capture needs)

Files: `src/dronekill.{h,cpp}`, `src/quest/plugins/q15t/src/dronekill_core.h`, `src/quest/plugins/q15t/src/dronekill_log.h`, `src/quest/tests/q15t/dronekill_test.cpp`, `src/quest/tests/q15t/mod/dronekill.sh`,
`tests/mutants.d/dronekill*.sh` (six files: one list, split so that each `Q15T_MUTANTS_ONLY=<file>` run fits the 10-minute tool limit).

Port of the source build's `the source build's dronekill` (drone kill / lock-on / gun mirror / chase / shot velocity) and of its
platform-free core the source build's dronekill_core. Only GOT word swaps on data pages and calls into libr15; **no code page is written**.

## Option and default

`Q15T_DRONEKILL`, default **ON** (#1625: the wave19a on-list turns on `dronekill2.on`). The source build is TEST ONLY: the source build's dronekill:1 ("dronekill.on (TEST ONLY ...)"),
and ("Off (no dronekill.on in the source build's data directory, no setprop debug.<app>.dronekill 1): the switch is read, nothing else"), the on-file
reader (QA turns it on with `dronekill2.on` = the words `kill lockon gun chase shot`). The plugin has no file switch (an
enabling switch is replaced by the option); when the option is on, `DroneKillWords{}` has all five words on, i.e. the source's `dronekill2.on`.
The five words are plain booleans of `DroneKillWords` (the main seat may expose them; the default struct is all-on).

## API

```
InstallResult-style:  DroneKillInstall InstallDroneKill(base, words, fns, slots = kDroneKillPinned)   // all words or none
bool                  ResolveDroneKillFns(base, &fns)    // six exports by name (dlsym), each required at base + its pinned vaddr
void                  DroneKillTick(game)                // game thread, once a frame; one atomic load while nothing is due
void                  DroneKillOnKilled(index)           // the 'K' feed (see "Not built")
const float*          DroneKillShotVelocity(int* remote) // the accessor dronemuzzle uses
bool                  DroneKillShotActive()              // the FireBulletLocal word is swapped
DroneKillStats        GetDroneKillStats()
```
Test seams: `SetDroneKillCallsForTest`, `SetDroneKillClockForTest`, `SetDroneKillShotForTest`, `SetDroneKillShotActiveForTest`,
`ResetDroneKillForTest`, `ResolveDroneKillFnsFromAddressesForTest`.

## Rows covered, mechanism per row

All four are `InstallImportHook(symbol, base, slot_vaddr, wrapper, &real, expected_vaddr)`: the slot must hold exactly `base + the real
function's ELF vaddr` or nothing is swapped. ELF vaddr = ReVault VA - 0x100000.

| Row | Function | JUMP_SLOT (ELF) | real fn (ELF) = ReVault VA, size | wrapper |
|---|---|---|---|---|
| R45 | `CEnableComponentNode::Enter` | 0x36cacf8 | 0x263b700 = 0x273b700, 424 | `EnterWrap` |
| R47 | `CR15NetAutoTargetCS::AttackTarget` | 0x36e6610 | 0x210b8c4 = 0x220b8c4, 1116 | `AttackWrap` (chase) |
| R48 | `CR15NetAutoTargetCS::FireBulletLocal` | 0x36c37a8 | 0x210bff8 = 0x220bff8, 1200 | `FireWrap` (lock-on, shot accessor) |
| (none) | `SComponentNodeUtils::Actor` | 0x36e2080 | 0x2059714 = 0x2159714, 544 | `ActorWrap` (pass-through; capture only while a drone Enter runs) |

The comparator slot (JUMP_SLOT 0x36f91f0 -> 0x1524cb4 = ReVault 0x1624cb4 `operator()`, 488 bytes) is only **read**: if it does not hold its
function, Component() look-ups are off (one WARNING), every other part still installs. The Actor slot is not a numbered row in
`R1_functions.md` (it lists R45, R47, R48 at lines 58-61; unverified whether Actor appears under another id); its slot is in
`R0_readelf_r.txt:124068`, the comparator's at `:135890`.

Swaps: Actor, Enter, FireBulletLocal (when `lockon` or `shot`), AttackTarget (when `chase`). Every slot is read before the first swap; a
slot that does not hold its pinned function refuses the whole install (`kUnexpectedOriginal`, ERROR `dronekill: OFF, nothing changed`);
a swap that fails puts the earlier ones back (WARNING; ERROR when one cannot be put back). Call `InstallDroneKill` **before**
`InstallGameTick` (the wrappers' state is final before the tick thread exists).

### What each wrapper does (all reads through `SafeRead`; on any doubt the real function runs untouched)

- **Enter** (`CEnableComponentNode::Enter(node, data)`, `data` = `{actor sym, space sym, system sym, component sym, enable}`): not the drone
  actor symbol (`kDroneActor` 0x10e40287a9bf5a13) -> the real call with the same arguments, one counter. A drone Enter: the **real
  Enter runs first**, unchanged arguments, with a thread-local capture armed; `ActorWrap` copies the two out-parameters (game space, SActor)
  of the Actor call whose symbol is the drone's. Then, with the capture: `FindComponentSystemMT` (const, resolved by name and required at its
  pinned address), the system's `Component()` slot (+0x1b8 -> +0x1b0) **only after** its words match the read shapes (7-word thunk, 54-word
  body whose one `bl` must land on the comparator's PLT stub 0xf43610), the component handle, and the table work under one mutex:
  * **kill** (R1'/R2/R3 of the core): the damageable ON opens a group; other components join the actor's group (or wait as pending);
    a 'K' marks the group dead and due; at the tick each component goes off through `DisableComponent(handle, 1)` (vtable +0x210, shape
    checked); a dead group's component enabled again is due again (R3).
  * **lockon** (F3): a touch interact (`kR14Touch`) turned OFF by the script is "deployed": re-enabled at the tick through
    `EnableComponent` (vtable +0x208, shape checked) once the script's disable is applied, held, and turned off again after the drone's first
    shot (FireBulletLocal binds the held record), only while its drone is live.
  * **gun** (F5): the gun's Enter is mirrored to the satellite and the port by extra real Enters on a COPY of the data (the script's data is
    never written); a port Enter that differs from the gun's state gets one more Enter with the gun's state.
  * A drone Enter in another game space than the table's is a level change: the group, touch, gun and chase tables are cleared.
* **FireBulletLocal(cs, handle, C3Vector velocity, remote)** (the vector is an HFA in s0-s2, passed on whole): marks the touch record of this
  handle locked on (lockon), then sets thread-locals (velocity, remote) around the real call and restores the outer ones afterwards (a shot
  inside a shot). `DroneKillShotVelocity` answers them; dronemuzzle reads it.
- **AttackTarget(cs, handle)** (chase, F7): the real call first, then (from the game's own records, loads only) the velocity toward the target
  (7 m/s cap, 7 m stand-off, gain 2, aim 0.5 m above), stored for the tick; at the tick it is written with `CPhysicsCS::SetWorldVelocity`
  **only if** the record is younger than 100 ms, the drone is still the same one (generation words of both handles, owners), the physics
  system's game space is inside the invoke-immediate window (`[[system+0x80]+0x368] != 0`), and the drone is locally controlled
  (`IsLocallyControlled`; fails closed: any unreadable or unknown thing is "not local" and writes nothing).
- **Tick**: `DroneKillTick(game)` is the source's net tick body (`source-build ec` called it at the end of each recvfrom on the game's socket); here it
  runs once per frame on the game thread. A disable/enable that would have to be queued (invoke-immediate word 0) is **not** done by the
  module (the queue push races its drain): it waits (due bits expire after 2 s).

## Evidence

Source build (read-only): the source build's dronekill:8 (Enter layout), `:24` (comparator slot rule), `:37-38` (stage 1, `kill`), `:49` (stage 2 words),
`:247` ActorWrap, `:387` stage-1 off, `:801-815` the four swapped words in order, `:921` word parsing; core the source build's dronekill_core (ported
verbatim except: namespace `q15t::dk`; the 'K' ring removed; `Comp` members carry initializers so every table is constant-initialised);
host test `native/the source build/tests/the source build's dronekill_test` (ported); the 'K' handler the source build's ec:185-215.

ReVault (binary 576, read this session in a read-only transaction: `functions`, `call_edges`, `instructions`, `xrefs` tables):
```
== functions at the pinned VAs (binary 576) ==
Actor | 2159714 | Actor | 544
BinarySearch cmp lambda | 1624cb4 | operator() | 488
CPhTarget::ResolveBody | 1aa27ac | ResolveBody | 104
CPhysicsCS::PrimaryBody | 1777d34 | PrimaryBody | 112
CPhysicsCS::SetWorldVelocity(handle,C3) | 177bec0 | SetWorldVelocity | 244
CR15NetPhysicsCS::IsLocallyControlled | 2275630 | IsLocallyControlled | 280
CTransformCS::ComputeLocalToWorldTransfQ | 17f6ef0 | ComputeLocalToWorldTransfQ | 144
FindCompSysMT const | 1904a10 | FindComponentSystemMT | 92
FindCompSysMT nonconst | 1904958 | FindComponentSystemMT | 184
R45 Enter | 273b700 | Enter | 424
R47 AttackTarget | 220b8c4 | AttackTarget | 1116
R48 FireBulletLocal | 220bff8 | FireBulletLocal | 1200
ref Disable NetDamageableCS | 2311868 | DisableComponent | 192
ref Enable CAbilityCS | 14e8cdc | EnableComponent | 188
ref Enable TouchInteractCS | 1e1f17c | EnableComponent | 188
```
The JUMP_SLOT of `FindComponentSystemMT` is the **non-const** one (0x1904958, 184 bytes); the module needs the const one (0x1904a10), so it
is looked up by name (`dlsym`) and required at `base + 0x1804a10`; the other five exports are checked the same way (`kFnSpecs` in
`dronekill.cpp`, vaddrs = ReVault - 0x100000).

Shape tables compared with ReVault (a one-off script comparing the words ReVault returned with the tables in `src/quest/plugins/q15t/src/dronekill_core.h`; quoted output):
```
7 54 48 47
Enable ref matches kEn: True exact= True        (CAbilityCS EnableComponent 14e8cdc, 47 words; first word f60f1df8 = 0xf81d0ff6, last c0035fd6)
Disable ref matches kDis: True exact= True      (NetDamageableCS DisableComponent 2311868, 48 words; last 40001fd6 = 0xd61f0040)
1043610 adrp x16, 0x37f9000 b03d01d0            (the comparator's PLT stub: adrp / ldr x17,[x16,#0x1f0] / add / br x17)
1043614 ldr x17, [x16, #0x1f0] 11fa40f9
1043618 add x16, x16, #0x1f0 10c20791
104361c br x17 20021fd6
cmp insns 122
thunks 270 exact kThunk 270                     (every 28-byte "Component" function in binary 576)
bodies 270 BodyOk 270 bad []                    (every 216-byte "Component" function; bl must reach 0x1043610)
```
(Words as printed by ReVault: the Enable body has `022d40f9` = `ldr x2,[x8,#0x58]` (OnComponentEnabled) then `40001fd6` = `br x2` (the tail call), and ends `f37b42a9 f55341a9 f60743f8 c0035fd6` (ret); the Disable body has `023140f9` = `ldr x2,[x8,#0x60]` then `f75bc3a8`, `40001fd6` = `br x2` as its last word; both equal the tables `kEn` / `kDis` word for word.) The R1_functions.md rows: `R45 ... JUMP_SLOT 0x36cacf8`, `R47 ... 0x36e6610`,
`R48 ... 0x36c37a8` (a source-build note, not in this tree). Callees of the three functions read in ReVault `call_edges`:
FireBulletLocal calls `0220c9c4` (FireBullet) and `0190a384` (WorldTeleport); AttackTarget calls `0220bff8` (FireBulletLocal) and
`017f6ef0` (ComputeLocalToWorldTransfQ), `01777d70`, `01aa27ac` (ResolveBody); Enter calls `02159714` (Actor) and `01904958`
(FindComponentSystemMT, non-const).

**Unverified in ReVault (copied from the source build, labelled so):** the struct offsets of the auto-target system (`kAtIdx` 0xc8 ... ),
the damageable system (`kDm*`), the physics system (`kPh*`), `CPhBody` position at +0x8f8, `kNetPhysicsPhys` 0x170, and the symbol hashes
(`kDroneActor`, `kDamageable`, `kR14Touch`, `kNcaModel`, `kGun`, `kSatellite`, `kPort`, `kNetPhysics`). They are the source build's
values; the host test pins them as literals (`TestPinnedConstants`) so a change is loud, but I did not re-derive them.

## Behaviour on the host test

`src/quest/tests/q15t/dronekill_test.cpp` runs the module code against a fake game library laid out as the module reads it (component systems with the real
read-shape words, a fake auto-target system for chase, a fake slot page with real `mprotect` and `/proc/self/maps` for the GOT swaps, real
`SafeRead`). The vtable-slot calls into game code are replaced through the module's call seam (their words are aarch64 data on the host).

## Logging

One structured line per install hook (INFO ok, ERROR refused), one summary `dronekill: on (QA)`, WARNING on a comparator slot that does
not hold its function, ERROR for a missing export or a missing `FindComponentSystemMT`. Behaviour lines: `dronekill: release (damageable
on)`, `dronekill: component`, `enter note` (why a drone Enter was not tracked), `re-enabled in a dead group`, `K`, `component off`, `tick`
(WARNING when due bits expired or waited), `touch deployed / on again / locked on`, `gun mirrored`, `port follows the gun`, `chase`
(first 8). Every kind is capped (note 24, enter 160, off 64, K 48, f357 96, chase 8) and the line that reaches a cap says so once (`log cap
reached kind=...`): a silent stop would look like the module went quiet. No per-call log line on the hot wrappers (counters only). No
secrets are logged.

## Prediction (written before any headset run)

With `Q15T_DRONEKILL=1` (all five words) in an EC drone match on the pinned build:
1. Install: four `dronekill hook install ... status=ok` lines and `dronekill: on (QA) ... component_lookups=on (read shapes only)`; no ERROR.
2. At a drone's spawn: `dronekill: release (damageable on) index=<n> group=<g>` then `dronekill: component ... to=group <g>` lines (a few, <= 20 per
   group); a few `enter note` lines at most.
3. Gun: when the script shows the gun, one `dronekill: gun mirrored (satellite and port follow) gun=shown`; the satellite and port stay in step
   with the gun (they were the bug).
4. Lock-on: `touch deployed` at the drone's spawn, `touch on again` within ~1 frame to 1 s, and after the drone's first shot `touch locked on
   ... result=off`: the drone is grabbable until it locks on, then not. Never `touch locked on` for a dead drone's touch.
5. Chase: the first eight `dronekill: chase drone=<h> dist_m=<d> speed_ms=<s> locally_controlled=1|0` lines; drones with `locally_controlled=1`
   close to about 7 m of their target; `locally_controlled=0` drones are not moved by this peer.
6. Kill: **no `K` line ever appears** (the 'K' feed is not in the plugin), so no group is ever shut down by this module, and none of the
   `component off` lines appear. (Falsified only if a later module calls `DroneKillOnKilled`.)
7. No crash in any of the four functions; frame time unchanged (one atomic load per tick when nothing is due; counters only per call).

## Falsifiers

- `dronekill hook install ... status=unexpected_original` or `dronekill: OFF, nothing changed`: a slot does not hold its pinned function on
  this build (nothing was swapped).
- `dronekill: a function is missing or not at its pinned address`: an export is elsewhere than the ReVault read says; nothing is swapped and
  nothing is called.
- `dronekill: the comparator slot does not hold its function`: Component() look-ups are off; all tracking lines say `not looked up`.
- `enter note ... not looked up (its Component() is not the read shape)` for most systems: the shape tables do not match this build.
- Satellite or port out of step with the gun after the mirror line: the extra Enters on a data copy are not equivalent to the script's own.
- A drone that jumps or stutters with `locally_controlled=1`: the chase velocity is written outside the window or against a recycled handle.
  A drone that never moves with `locally_controlled=1` lines present: the tick is not inside the invoke-immediate window (**unverified**: whether
  the game tick runs inside that window; the source's net tick ran in `recvfrom`) - then every `tick` WARNING says `waiting_invoke_immediate_0>0`.
- A `touch locked on` for a drone that is dead, or a grabbable drone after its first shot: the lock-on binding is wrong.
- Any crash with a frame in `dronekill`: a read path that is not guarded (all are `SafeRead`).

## Needs a word patch / not built

- **Needs a word patch: none.** Every row is a data-page GOT swap or a call through a vtable slot / export.
- **Not built:**
  * the **'K' feed**: in the source, `source-build ec` passes each server "drone killed" datagram to `OnKilled`. The plugin has no module with the recvfrom
    row (R43); until one calls `DroneKillOnKilled(index)`, `kill` never fires. lock-on, gun, chase and shot do not need it.
  * the source's recvfrom-based net tick (replaced by the game tick; see "Unverified" above).
  * file / property switches (replaced by the option).
- **Unverified:** (1) the game tick versus the invoke-immediate window (above); (2) the struct offsets and symbol hashes listed under
  Evidence; (3) the register contracts of the vtable-slot calls beyond what the source does (the module does exactly what the source does).
- **Untestable on the host** (documented, not mutated): the rollback of a swap whose restore of page protection failed
  (`kRestoreFailed`, needs an `mprotect` failure), the extra handle compares in the tick's `ReOnDone`/`OffDone` guards (they differ from the
  core's own state guards only when a slot is reassigned to another handle inside one engine call), and the thread-local nested-capture restore
  in the gun mirror (it differs from the single-level case only for a drone Enter nested in another drone's Enter).
- **Equivalent mutants** (not kept in the lists; the guarded value cannot differ from the neighbouring check): a `!ptr` test directly in
  front of a `SafeRead(ptr + n)` (the read of a low address fails anyway), the `ok &&` in front of a `Rd` whose failure value is 0, the
  `data == nullptr` test in front of a `SafeRead(data)`, `cap.have` in front of `cap.ok`, the `g_fns.find != nullptr` guard after the install
  required it, the `if (g_lockon)` guards around table operations that are empty without the word, a `CacheShape` duplicate check
  (single-threaded), the `g_gun`-gated `Set` in the K path (the table is only read with the word on), the `g_chase`-gated tick call.

## Tests and mutants

Host test (`Q15T_MODULE=dronekill bash src/quest/tests/q15t/run_host_tests.sh`, 2026-10-28):
```
dronekill_test: all checks passed
dronekill: arm64 compile ok, no .init_array
```
(the arm64 step is an NDK r26d clang++ compile with `-fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra -Werror -O2` and an
`.init_array` check; the full `bash src/quest/tests/q15t/run_host_tests.sh` also passes, rc=0.) The test pins the tables by FNV-1a hash and every offset /
symbol / constant as a literal, ports the source's core tests, and drives the real module against the fake game for install (pinned vaddrs on a
58 MB fake base, all-or-none, rollback), Enter / K / tick, R3, lookups and shapes, gun mirror, lock-on, shot accessor (nested), chase (about 40
scenarios), log caps, the wrappers with no real function, and the function-address check.

Mutants (`Q15T_MUTANTS_ONLY=<file> bash src/quest/tests/q15t/mutants.sh`, one run at a time, each file within the 10-minute tool limit): the list is split in six
files `tests/mutants.d/dronekill{,_b,_c,_d,_e,_f}.sh`, **324 mutants, 324 killed, 0 survived** (62 / 62 / 62 / 62 / 62 / 14). Final run output
(`exit=0` for each file; `dronekill_d` was rerun alone after the last test fixture change and also gave 62 killed, `exit=0`):
```
== dronekill   MUTANT ...: killed (62)   exit=0
== dronekill_b ... killed (62)   exit=0
== dronekill_c ... killed (62)   exit=0
== dronekill_d ... killed (62)   exit=0
== dronekill_e ... killed (62)   exit=0
== dronekill_f ... killed (14)   exit=0
```
Constants are mutated in groups (one `sed` with several substitutions: pinned slots per row, function vaddrs, symbols, times, offsets); the
literal-pin test names the exact constant. Survivors found on the way and what killed them: kill-test gaps for the touch actor, the
touch-table free-slot choice, a gun-like component in another system, the K line's masked index, plus the whole chase block (handles, owners,
generations, stale velocities), all caps, rollbacks and wrappers without a real function. Not mutated (documented above): equivalent mutants and
three host-untestable defence-in-depth guards.

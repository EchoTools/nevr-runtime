# dronekill (rows R45, R47, R48, and the Actor slot Enter's capture needs)

A combat drone that the server says died goes off, a drone that has locked on stays grabbable until it fires, its gun, satellite and port show as
one, it chases its target, and its shot hands dronemuzzle the shot's velocity. Only hooks through the hook manager and calls into libr15;
**no code page is written**.

Files: `src/quest/combat/dronekill.{h,cpp}`, `dronekill_core.h` (the platform-free rules), `dronekill_targets.h` (the pins), `thread_word.h`
(the per-thread words); tests `src/quest/tests/dronekill_test.cpp` (`just test-quest-combat dronekill`), the pins in `src/quest/tests/got_pinned_test.cpp`
(`just test-quest-hooks-pinned`); mutants `src/quest/combat/mutants.d/dronekill{,_b,_c,_d,_e,_f}.sh`.

Origin: the reference build's `runtime/native/afkxr/afk_dronekill.cpp` and afk_dronekill_core.h (kotorvr's echoquest-combat), which port EchoCombat's
`plugin/dronefix.cpp` (LockedOn df 805-827, ChaseTarget df 1059-1095; the groups df 434-560, TouchLive df 778-793, the touch rule
df 731-835, the gun table df 700-729, the chase table df 1037-1095).

## Switch and default

`combat.dronekill` in `config.yaml`, a boolean, default **true**: the reference build ships `dronekill2.on` (all five words: kill, lockon, gun,
chase, shot) and the combat plugin imported in #1591 built the module on. `false` installs nothing. The five words are not keys: all five are on (`dronekill::Words{}`).
There is no switch file and no legacy file for this module.

`combat.dronekill_explosion` and `combat.dronemuzzle` default **true** too (`src/quest/sentinel/quest_config.h`, `dronekillExplosion` and
`dronemuzzle`), and `combat.dronekill_hold` defaults **false** (the same file, `dronekillHold`). The two true ones depend on the scope:
`combat.dronemuzzle` reads it through `dronekill::InCombatScope()` (`src/quest/combat/dronemuzzle.cpp`), and the explosion runs in the tick. So
the leave window below is the default behaviour of the combat APK, not an effect of the hold alone.

## What it hooks

Four CallbackThunks on JUMP_SLOTs of libr15 (`dronekill_targets.h`), installed through `sentinel::InstallThunk` by the normal step of
`combat_install.cpp` (after refresh_rate, before dronemuzzle). Each target names the pinned function as the expected original.

| Row | Function | JUMP_SLOT (ELF) | real fn (ELF) = disassembly VA, size | handler |
|---|---|---|---|---|
| R45 | `CEnableComponentNode::Enter` | 0x36cacf8 | 0x263b700 = 0x273b700, 424 | `EnterHandler` |
| R47 | `CR15NetAutoTargetCS::AttackTarget` | 0x36e6610 | 0x210b8c4 = 0x220b8c4, 1116 | `AttackHandler` (chase) |
| R48 | `CR15NetAutoTargetCS::FireBulletLocal` | 0x36c37a8 | 0x210bff8 = 0x220bff8, 1200 | `FireHandler` (lock-on, shot accessor) |
| (none) | `SComponentNodeUtils::Actor` | 0x36e2080 | 0x2059714 = 0x2159714, 544 | `ActorHandler` (capture only while a drone Enter runs) |

Each symbol has exactly one relocation in the store libr15 (`llvm-readelf -rW`, sha256 8dd9a961...): no GLOB_DAT word bypasses the slots. The
comparator slot (JUMP_SLOT 0x36f91f0 -> 0x1524cb4 = disassembly 0x1624cb4 `operator()`, 488 bytes) is only **read**: if it does not hold its
function, Component() look-ups are off (one Warning), every other part still installs.

The step: the libr15 the sentinel attached (its build ID must be the pinned one), the six functions the module calls (`dlsym`, each required at
`base +` its pinned vaddr, `engine_export_missing` otherwise), the frame tick (`tick::Add`, `tick_table_full` otherwise), then the hooks: Actor,
Enter, FireBulletLocal (when `lockon` or `shot`), AttackTarget (when `chase`). Every wanted slot is read before the first install and must hold
its pinned function (`slot_not_pinned_function`); a failed install removes the earlier ones (`GotHook::Remove`, a `combat_dronekill_rollback`
Warning, an Error when one cannot be removed). Last, the kill feed: `ec::OnDroneKilled(&dronekill::OnKilled)` (ec.h).

### What each handler does in a combat map (all reads through `SafeRead`; on any doubt the real function runs untouched)

- **Enter** (`CEnableComponentNode::Enter(node, data)`, `data` = `{actor sym, space sym, system sym, component sym, enable}`): not the drone
  actor symbol (`kDroneActor` 0x10e40287a9bf5a13) -> the real call with the same arguments, one counter. A drone Enter: the **real
  Enter runs first**, unchanged arguments, with a per-thread capture armed (combat/thread_word.h); the Actor handler copies the two out-parameters (game space, SActor)
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
  handle locked on (lockon), then sets a per-thread shot frame (velocity, remote) around the real call and restores the outer ones afterwards (a shot
  inside a shot). `dronekill::ShotVelocity` answers it; dronemuzzle reads it.
- **AttackTarget(cs, handle)** (chase, F7): the real call first, then (from the game's own records, loads only) the velocity toward the target
  (7 m/s cap, 7 m stand-off, gain 2, aim 0.5 m above), stored for the tick; at the tick it is written with `CPhysicsCS::SetWorldVelocity`
  **only if** the record is younger than 100 ms, the drone is still the same one (generation words of both handles, owners), the physics
  system's game space is inside the invoke-immediate window (`[[system+0x80]+0x368] != 0`), and the drone is locally controlled
  (`IsLocallyControlled`; fails closed: any unreadable or unknown thing is "not local" and writes nothing).
- **Tick**: `dronekill::Tick(game)` runs the reference build's net tick body (its EC module called it at the end of each recvfrom on the game's socket)
  once per frame on the game thread (combat_tick.h), and asks the combat level answer (combat_scope.h) for the scope every 32 frames. A disable/enable that would have to be queued (invoke-immediate word 0) is **not** done by the
  module (the queue push races its drain): it waits (due bits expire after 2 s).

## Arena (same APK)

Arena and combat run on the same APK, and Actor and Enter are script-node functions every level's scripts call, so the module acts only while its
scope is on. The scope is on while a combat map is loaded and no Echo Arena level is, and for the leave window after the first no-combat-map answer (below, which bounds it against the map's end):
`dronekill::Tick` asks the combat level answer every 32 frames (`kScopeFrames`), one `combat_scope.h`
`LastLook()` record judged by `InCombatMap(look)` (a combat map, no Echo Arena level, fresh), whose level the change line names, and keeps the answer in
one atomic, read by `InCombatScope()` (false before the first tick: fail closed). The combat lobby is not the scope (no drone is in it as far as
this module knows; `dronekill_test` checks `mpl_lobby_b_combat` is out of scope). The module is one of the scope row's readers (`combat.dronekill` is in
`ScopeNormal()` in `combat_install.cpp`), and its step refuses with `scope_not_live` when the answer cannot change.

Out of scope: Enter and FireBulletLocal call the game's function and return before anything else (no shot frame, so dronemuzzle passes too);
AttackTarget returns after its call; Actor returns after its call when no drone Enter is in flight on any thread (always, out of scope); a
'K' is dropped; once the scope has ended the tick does nothing. The scope ends on `kLeaveAsks` = 3 no-combat-map answers in a row (a look
cannot tell an unreadable game from one with no combat map; entering takes one), and then it empties the group, touch, gun and chase tables, so
nothing from a combat map is acted on after that. Until then the scope is still on (see "The leave window").

### The leave window

From the first no-combat-map answer until the third, which is two 32-frame asks later, the scope stays on for up to 64 frames. In that window:

- the tick runs its due work every frame: the due components are taken and turned off one at a time (`OffOne`, through `Revalidate`), and the
  touch, chase and explosion ticks run under their own words (`TickBody` in `src/quest/combat/dronekill.cpp`). `dronekill_test`
  (`TestScopeEndsAtTheLevelChange`) pins the due work running in the window and not after the scope ends;
- the in-scope bodies of the four hooks and `OnKilled` run for each call that passes `scope::LookIsFresh()`, which the scope row's own look keeps fresh (its `Refresh`, not this module's asks). No
  test pins them in the window; this follows from the code (`g_inScope` is set until the scope ends).

Each kind of due work has its own guard (`dronekill.cpp`), and none of them reads whether the level is still loaded (from the code read; that an
unloaded game space cannot keep a matching owner word is not established):

- a component turned off or on (`OffOne` and `OnOne`: the due components, the held components an explosion releases, and the touch's off and on)
  goes through `Revalidate`. It passes only when the system's owner word (read with `SafeRead`) still equals the recorded owner, the system is
  found again in that space, its `Component()` has the read shape and returns the recorded handle. Then `DisableComponent`'s (or
  `EnableComponent`'s) shape is checked, and an invoke-immediate word that is 0 makes the component wait.
- a chase velocity (`ChaseTick`) goes through `ChaseStillOk`: the physics system's owner word equals the recorded one, and the handles are live.
  Then the physics system's owner word must equal the recorded owner again, and that owner's invoke-immediate flag must be nonzero, before
  `set_vel`. `Revalidate` is not called.
- an explosion (`FxTick`, then `RunExplosion`) goes through `EntryOk` for both drone entries (each must be the expected entry of the drone
  library, by its module and offset) and a `SafeRead` of the context's state word. `Revalidate` is not called; the components it releases go off
  through `OffOne`, as above.
- a touch's off (`TouchTick`) first checks that the component or its actor is still in the table (`LiveComp`, `LiveActor`), then calls `OffOne`.

So on an arena level, for up to 64 frames after the first no-combat-map answer, the due work and the in-scope bodies run on the combat map's
recorded components. That path is the arena's cost in this window; the out-of-scope path counted under "Arena cost" below is the cost once the
scope is off. The window is counted from that answer, not from the map's end. The asks are 32 frames apart (`n % kScopeFrames`, `Tick`), and the
first look that reads the map's end comes at most 250 ms after the end while the game tick runs (`kLookEveryMs`, the scope row's look). The
first no-combat-map ask comes at most 31 frames after that look, and the scope ends 64 frames after that ask. So the due work's last frame is at
most 94 frames after the first frame whose look reads the end. This assumes one `Tick` call per frame (`dronekill.h` describes the game's frame
tick that way; the rate is not measured here).

Lines in the window: one `combat_dronekill_scope_wait` (Debug) per no-combat-map answer that does not yet end the scope.

The freshness test is made at call time (`src/quest/combat/README.md` rule 9). `InCombatScope()` reads the atomic that the tick's last ask stored, and
only an ask writes it, so while the game tick is stopped it still says combat. Every in-scope body (`EnterInScope`, `FireInScope`, `AttackInScope`) and
`OnKilled` therefore start with `scope::LookIsFresh()`: once the look is older than a second each hook calls the game's function as it was called and
does nothing else, and a 'K' is dropped. The tick's own work needs no such test: it runs right after the scope's refresh in the same loop, and it runs for as long as the scope is on
(the leave window above). A stale or missing look counts as no combat map at an ask that runs (`scope::InCombatMap` on the record), and leaving takes `kLeaveAsks` such asks
in a row; so when the tick runs again, a combat map that is still loaded keeps the tables, and a stale look has to be asked three times before the
scope ends. `dronekill_test` (`TestAStoppedTickPassesEveryCallThrough`) pins the stopped case: 60 s on the scope's clock with no tick leaves
`InCombatScope()` true, and an Enter, a shot, an attack and a 'K' each pass through with no look-up, counter, shot frame or line; the next frame after
the tick is back acts again. In a combat map the test costs each hooked call one clock read (31 to 38 ns on the x86 host, the scratch loop
`powerboost.md` "Arena" names); Enter runs once per enable/disable-component node, AttackTarget once per frame per targeting drone, so the added time
per frame is that rate times about 38 ns, and ADR 0038's 10 us a hook is reached only above about 260 calls a frame (not measured on a headset). The arena path reads no clock: `ExpectInert` counts the scope's clock reads and
wants zero. One `combat_dronekill_scope` line per change of scope, with each hook's call count over the scope that ended (the
measurement a headset run gives). `dronekill_test` (`TestArenaLevelsAreLeftAlone`, `TestScopeEndsAtTheLevelChange`) drives a drone's Enter,
an Actor, a shot, an attack, a 'K' and 65 ticks on `mpl_arena_a`, `mpl_lobby_b2`, `mpl_lobby_b_combat` and no level, and checks that each hook
called the game's function as it was called and nothing else happened: no look-up, no table entry, no disable, no velocity, no line.

### Arena cost (ADR 0038)

What each hook executes in an arena level, counted in the arm64 object NDK r26d clang++ makes of `dronekill.cpp` at -O2 (`llvm-objdump -d`),
on top of the game's own function; the thunk entry is `CallbackThunk::Entry`, shared by every thunk hook in the runtime:

| Hook | thunk entry to the handler | handler out of scope | per call | how often |
| --- | --- | --- | --- | --- |
| `CEnableComponentNode::Enter` | 23 instructions + `__aarch64_ldadd8_relax` (the call counter) | 7: `adrp`, `mov`, `ldrb` (the scope byte), `tbnz`, 2 `mov`, `br` (a tail call of the game's Enter) | 30 + the counter | once per enable/disable-component script node a level runs |
| `SComponentNodeUtils::Actor` | 35 + the counter | 24: a frame, the game's call, one relaxed `ldr` of the in-flight count, `cbz`, the return | 59 + the counter | once per node that looks an actor up (Enter's among them) |
| `CR15NetAutoTargetCS::FireBulletLocal` | 37 + the counter | 8: as Enter's, a tail call | 45 + the counter | once per auto-target shot (drones; none in arena as far as read) |
| `CR15NetAutoTargetCS::AttackTarget` | 23 + the counter | 15: a frame, the game's call, `ldrb`, `tbnz`, the return | 38 + the counter | once per frame per auto-target with a target |
| the frame tick (`combat_tick.h`) | | 23 instructions + the counter's `__aarch64_ldadd8_relax` on 31 frames of 32; on the 32nd also `RefreshScope` (one `LastLook()` of the combat level answer: a sequence-count read and a clock read, no read of the game; the level scan is the scope row's, four times a second while a reader is on) | | once per frame |

No system call, lock, allocation or log line on any hook path out of scope (in the leave window the paths run as in scope: see "The leave window"). Not measured on a headset: how often arena's scripts run Enter and
Actor (the `combat_dronekill_scope` line carries the counts of each scope when it ends).


## Evidence

Source build (read-only): the source build's dronekill:8 (Enter layout), `:24` (comparator slot rule), `:37-38` (stage 1, `kill`), `:49` (stage 2 words),
`:247` ActorWrap, `:387` stage-1 off, `:801-815` the four swapped words in order, `:921` word parsing; core the source build's dronekill_core (ported
verbatim except: namespace `nevr_quest::combat::dronekill::dk`; the 'K' ring removed; `Comp` members carry initializers so every table is constant-initialised);
host test `native/the source build/tests/the source build's dronekill_test` (ported); the 'K' handler the source build's ec:185-215.

Disassembly (read this session in a read-only transaction: `functions`, `call_edges`, `instructions`, `xrefs` tables):
```
== functions at the pinned VAs ==
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
`src/quest/combat/dronekill.cpp`, vaddrs = disassembly - 0x100000).

Shape tables compared with the disassembly (a one-off script comparing the words the disassembly returned with the tables in `src/quest/combat/dronekill_core.h`; quoted output):
```
7 54 48 47
Enable ref matches kEn: True exact= True        (CAbilityCS EnableComponent 14e8cdc, 47 words; first word f60f1df8 = 0xf81d0ff6, last c0035fd6)
Disable ref matches kDis: True exact= True      (NetDamageableCS DisableComponent 2311868, 48 words; last 40001fd6 = 0xd61f0040)
1043610 adrp x16, 0x37f9000 b03d01d0            (the comparator's PLT stub: adrp / ldr x17,[x16,#0x1f0] / add / br x17)
1043614 ldr x17, [x16, #0x1f0] 11fa40f9
1043618 add x16, x16, #0x1f0 10c20791
104361c br x17 20021fd6
cmp insns 122
thunks 270 exact kThunk 270                     (every 28-byte "Component" function in libr15)
bodies 270 BodyOk 270 bad []                    (every 216-byte "Component" function; bl must reach 0x1043610)
```
(Words as printed by the disassembly: the Enable body has `022d40f9` = `ldr x2,[x8,#0x58]` (OnComponentEnabled) then `40001fd6` = `br x2` (the tail call), and ends `f37b42a9 f55341a9 f60743f8 c0035fd6` (ret); the Disable body has `023140f9` = `ldr x2,[x8,#0x60]` then `f75bc3a8`, `40001fd6` = `br x2` as its last word; both equal the tables `kEn` / `kDis` word for word.) The R1_functions.md rows: `R45 ... JUMP_SLOT 0x36cacf8`, `R47 ... 0x36e6610`,
`R48 ... 0x36c37a8` (a source-build note, not in this tree). Callees of the three functions read in the disassembly `call_edges`:
FireBulletLocal calls `0220c9c4` (FireBullet) and `0190a384` (WorldTeleport); AttackTarget calls `0220bff8` (FireBulletLocal) and
`017f6ef0` (ComputeLocalToWorldTransfQ), `01777d70`, `01aa27ac` (ResolveBody); Enter calls `02159714` (Actor) and `01904958`
(FindComponentSystemMT, non-const).

**Unverified in the disassembly (copied from the source build, labelled so):** the struct offsets of the auto-target system (`kAtIdx` 0xc8 ... ),
the damageable system (`kDm*`), the physics system (`kPh*`), `CPhBody` position at +0x8f8, `kNetPhysicsPhys` 0x170, and the symbol hashes
(`kDroneActor`, `kDamageable`, `kR14Touch`, `kNcaModel`, `kGun`, `kSatellite`, `kPort`, `kNetPhysics`). They are the source build's
values; the host test pins them as literals (`TestPinnedConstants`) so a change is loud, but I did not re-derive them.

## Falsifiers

- `combat_dronekill_hook` with `"status":"original_mismatch"`, or `combat_dronekill_install` with `"result":"refused"`: a slot does not hold its pinned function on
  this build (nothing was swapped).
- `combat_dronekill_function` (Error): an export is elsewhere than the disassembly read says; nothing is swapped and
  nothing is called.
- `combat_dronekill_comparator` (Warning): Component() look-ups are off; all tracking lines say `not looked up`.
- `combat_dronekill_note` with `"why":"system not looked up (its Component() is not the read shape)"` for most systems: the shape tables do not match this build.
- Satellite or port out of step with the gun after the mirror line: the extra Enters on a data copy are not equivalent to the script's own.
- A drone that jumps or stutters with `locally_controlled=1`: the chase velocity is written outside the window or against a recycled handle.
  A drone that never moves with `locally_controlled=1` lines present: the tick is not inside the invoke-immediate window (**unverified**: whether
  the game tick runs inside that window; the reference build's net tick ran in `recvfrom`) - then every `combat_dronekill_tick` Warning says `waiting_invoke_immediate_0>0`.
- A `combat_dronekill_touch` `"step":"locked_on"` for a drone that is dead, or a grabbable drone after its first shot: the lock-on binding is wrong.
- Any crash with a frame in `dronekill`: a read path that is not guarded (all are `SafeRead`).
- In an Echo Arena match on the combat APK: any `combat_dronekill_*` line other than `combat_dronekill_scope` with `"in_combat":0` and the process-end `combat_dronekill_stats`, or a drone, a
  grabbable object or a bullet that behaves differently from the arena APK: the scope does not hold.

## Lines (sentinel log)

- `combat_dronekill_hook` `{"hook":"<name>","status":"installed|<GotStatus>"}`: Info when installed, Error otherwise. Plus the manager's own `got_hook` line.
- `combat_dronekill_install`: `"result":"installed"` with the five words, `explosion` (`combat.dronekill_explosion`), `hold` (`combat.dronekill_hold`), `fire_bullet_local`, `attack_target` (`hooked|not_hooked`) and
  `component_lookups`; `"result":"off"` (`off_config`); `"result":"refused"` with `reason` (the status token), `why` and the consequence (Error).
- `combat_dronekill_link`: the kill feed is registered with ec. `combat_dronekill_tick_first`: the frame tick runs.
- `combat_dronekill_scope` `{"in_combat":0|1,"level":"0x..","actor_calls":n,"enter_calls":n,"fire_calls":n,"attack_calls":n,"over_ms":n}`: one per change. `level` is
  the level of the look that decided it (`combat_scope.h` `Look::level`: the Echo Arena level, else the combat map, else the combat lobby; 0 none).
- `combat_dronekill_scope_wait` `{"no_level_asks":n,"needed":3}`: Debug, one per no-combat-map answer that does not yet end the scope (the leave window).
- `combat_dronekill_stats` `{"enter_drone":n,"enter_other":n,"fires":n,"attacks":n,"ticks":n,"due_runs":n,"capturing":n,"expired":n,"explosions":n,"held":n,"final":0|1}`: Debug every 16384 drone Enters; Info once at the end of the process when the module is installed (zero counts when no combat map was seen). `expired` counts the drone ends that found their group live (`combat_dronekill_expired`); `explosions` and `held` are the drone explosion's (docs/quest/combat/drone_explosion.md, with its own lines).
- `combat_dronekill_expired` `{"index":n,"group":n,"tid":n}`: Info, the drone's end: its damageable went off while its group was live. A drone that K killed and then ends writes none and is not counted. The first 16 of a run.
- `combat_dronekill_log_cap` `{"lines":"expired","cap":16}`: Warning, once, written by the drone end past the cap; the stats record goes on counting.
- `combat_dronekill_function` (Error: a function missing or not at its pin), `combat_dronekill_comparator` (Warning), `combat_dronekill_chase_off` (Error).
- Acting lines, each kind capped (note 24, enter 160, off 64, K 48, touch/gun 96, chase 8) with one `combat_dronekill_cap` line at the cap:
  `combat_dronekill_release`, `_component`, `_note` (why a drone Enter was not tracked), `_reenabled`, `_k`, `_off`, `_tick` (Warning when due
  work expired or waited), `_touch` (`step` `deployed|on_again|locked_on`), `_gun` (`step` `mirrored|port_follows|missing`), `_chase`.
No per-call line on a hook path out of scope; no secret is logged.

## Headset row

What a headset run must show, on the combat APK with `combat.dronekill` absent (default true):
1. Boot: four `combat_dronekill_hook` lines with `"status":"installed"`, `combat_dronekill_install` `"result":"installed"`, `combat_dronekill_link`,
   `combat_dronekill_tick_first`, and a `combat_install_module` record for dronekill with `"outcome":"installed"`.
2. An arena match: one `combat_dronekill_scope` `"in_combat":0` (or none, if the first ask already found no combat map: the scope starts out of
   combat), no other `combat_dronekill_*` line except the process-end `combat_dronekill_stats` (`"final":1`); its call counts on the next change are the arena rates (Enter and Actor per script node).
3. A combat match against drones with an EC server: `combat_dronekill_scope` `"in_combat":1`, `combat_dronekill_release` and `_component` lines at a
   drone's spawn, `_touch` `deployed` / `on_again` / `locked_on`, `_gun` `mirrored`, `_chase`, and with the server's 'K' datagrams `_k`
   `"result":"shut_down"` then `_off` `"result":"OFF"`.
Not shown on this machine: any of it (no headset run in this unit).

## Tests and mutants

`dronekill_test` (the `combat` host-test group) runs the real module and its four thunks, installed by the real GotHook into four JUMP_SLOTs of a
libr15 image built in memory, against a fake game laid out as the module reads it (component systems with the real read-shape words, a fake
auto-target system, real `SafeRead`). Its file header says which checks carry the combat plugin's test's assertions (#1591) and which are new; the fixture, the log form and the
API names are the runtime's. Mutants: the six lists, run with
`src/quest/combat/mutants.sh dronekill` (and `dronekill_b` .. `dronekill_f`, each naming `dronekill_test`); the results are in the PR.
Not mutated: equivalent mutants (a `!ptr` test in front of a `SafeRead(ptr + n)`, the `g_fns.find` guard after
the install required it, and the like).

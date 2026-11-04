# Module `dronemuzzle` (rows R49, R50)

Files: `src/dronemuzzle.{h,cpp}`, `src/quest/plugins/q15t/src/dronemuzzle_core.h`, `src/quest/tests/q15t/dronemuzzle_test.cpp`, `src/quest/tests/q15t/mod/dronemuzzle.cmake`,
`src/quest/tests/q15t/mutants.d/dronemuzzle.sh`. Shares `src/quest/plugins/q15t/src/dronekill_log.h` with dronekill.

Port of Ducky's reference combat build's `dronemuzzle` (drone fix F4 + F6: a drone's bullets leave the drone's
gun, not the drone's middle) and its core `dronemuzzle_core`. Two GOT word swaps on data pages; the two slots get **naked arm64
thunks** (the source's mechanism). **No code page is written.**

## Option and default

`Q15T_DRONEMUZZLE`, default **ON** (#1625: the wave19a on-list turns on `dronemuzzle.on`). The source is TEST ONLY: the source build's dronemuzzle:1 ("dronemuzzle.on (TEST ONLY ..."), `:29` ("Off (no
dronemuzzle.on in the source build's data directory): the switch is read, nothing else"). The plugin has no file switch. **It needs dronekill**
(`Q15T_DRONEKILL` with `shot` or `lockon`, the default words): the thunks act only while `DroneKillShotVelocity` answers non-null, i.e. inside
a drone shot's `FireBulletLocal`. Without dronekill's FireBulletLocal word `InstallDroneMuzzle` still installs, logs a WARNING (`shot_source=NONE
(dronekill off): the thunks only pass through`), and every call passes straight through.

## API

```
DroneMuzzleInstall   InstallDroneMuzzle(base, slots = kDroneMuzzlePinned, wrappers = DroneMuzzleThunks())   // both words or none
DroneMuzzleWrappers  DroneMuzzleThunks()   // the two naked thunks on arm64; {0, 0} on the host (the install then refuses, loudly)
DroneMuzzleStats     GetDroneMuzzleStats()
DroneMuzzleTick      DroneMuzzleTick(game)   // once a frame on the game thread: feeds the frame-time average (AddGameTick)
extern "C" (hidden)  q15t_dm_teleport(gs, actor, pose, reason)  q15t_dm_fire(bcs, handle, gun, damage, quat, vel, remote, flag)
                     q15t_dm_{tp,fire}_{ret,next}   // the words the thunks read: std::atomic<uintptr_t>, constant-initialised
```
Wiring for the main seat: `InstallDroneKill` first, then `InstallDroneMuzzle`, both before `InstallGameTick`; `AddGameTick(DroneKillTick)`
before `InstallGameTick` as well, and `AddGameTick(DroneMuzzleTick)` likewise (this tree's `plugin.cpp:509` registers it with
`AddModuleTick("dronemuzzle", &DroneMuzzleTick, &want_tick)`). No CMake edit is needed by this module (no new source other than the two
.cpp files listed; `dronekill_log.h` is header-only).

**Frame time.** `DroneMuzzleTick` runs once a frame on the game thread (`dronemuzzle.cpp:237-241`). It takes the time since its last
call from `NowNs()` (CLOCK_MONOTONIC) and feeds that interval to `FrameDt`: an interval in (1/240 s, 0.1 s) moves the running average
0.9/0.1, and the first call only records the time. Until the second call the average is `kDefaultFrameDt` = 1/90 s
(`dronemuzzle.cpp:41`). Each shot's back-off uses `ClampDt` of that average (1/120 s to 1/30 s). The game's own delta is not read (see Open).

## Rows covered, mechanism per row

| Row | Function | JUMP_SLOT (ELF) | real fn (ELF) = disassembly VA, size | acts on the call that returns to |
|---|---|---|---|---|
| R49 | `CncaGameSpace::WorldTeleport` | 0x36cfa18 (+ GLOB_DAT 0x370bcc0 not swapped) | 0x180a384 = 0x190a384, 236 | ELF 0x210c22c = disassembly 0x220c22c |
| R50 | `CR15NetBulletCS::FireBullet` | 0x36f3cf8 | 0x210c9c4 = 0x220c9c4, 976 | ELF 0x210c2a4 = disassembly 0x220c2a4 |

Both are called from `CR15NetAutoTargetCS::FireBulletLocal` (dronekill R48) among others (WorldTeleport has 25 callers, FireBullet 2), so the
wrapper must leave every other caller completely untouched: it needs the caller's registers and a check of the return address. That is the
thunk:

```
bti c
x16 = ldar [q15t_dm_*_ret]        ; the disassembly-read return address inside FireBulletLocal, published before the slot store
cmp x30, x16 ; b.eq 1f
x16 = ldar [q15t_dm_*_next] ; br x16   ; every other caller: x0..x7, s0, x30, sp exactly as the caller left them (only x16 is used)
1: stp x29, x30, [sp, #-16]! ; mov x29, sp ; bl q15t_dm_teleport / q15t_dm_fire ; ldp x29, x30, [sp], #16 ; ret
```
The `bl` keeps x0..x7 and s0 as the caller passed them, so the plain C++ function sees the original arguments. The thunks are
`__attribute__((naked))` functions with inline asm in `dronemuzzle.cpp`, under `#if defined(__aarch64__)`; the logic is in the two plain functions
(`q15t_dm_teleport`, `q15t_dm_fire`) which the host test calls; the thunk's decision is mirrored by two host stand-ins in the test (a C++
return-address compare), which prove nothing about the asm: the asm is checked by the cross-compile and by the comparison below.

- **F4, `q15t_dm_teleport`** (df 916-941): a drone shot (`DroneKillShotVelocity` non-null) reads the 32-byte pose (`CTransfQ`, passed by
  invisible reference in x2) with `SafeRead`; the spawn = the pose's position (+0x10) plus the muzzle offset `(0.000024, 0.075634, 0.147283)`
  in the aim basis (Z = v/|v|, X = (z, 0, -x)/|..|, Y = Z x X; `dronemuzzle_core.h`), then back along the aim by min(|v| dt, 0.25 m), dt the running average of host frame intervals (`FrameDt`, then `ClampDt`); the real function gets
  a **copy** with that position (the caller's pose is never written). |v| < 1e-4, or an unreadable pose: the call goes through untouched.
- **F6, `q15t_dm_fire`** (df 943-1019): the real FireBullet first; then for a drone shot with 1e-6 < v^2 < inf: the bullet record
  (`[bcs+0xf8] + ldrh([bcs+0xc8] + (handle & 0xffff) * 4) * 0x298`, the lookup FireBullet itself just made) gets shooter velocity (+0x11c) = 0 and
  shooter position (+0x128) = the record's spawn (+0x134), after readability probes on the three fields (a recycled handle or a short page:
  nothing is written). No effect on any other caller.

Install: both slots are read before either is swapped; a slot not holding its pinned function refuses both (`kUnexpectedOriginal`, ERROR
`dronemuzzle: OFF, nothing changed`). The return and next words are published before the swap (release store), so a thunk can never run before
its words are set (a 0 return word cannot match `x30`). If the second swap fails the first is put back (WARNING; ERROR if it cannot be).

## Evidence

Source build (read-only): the source build's dronemuzzle:7-10 (F4/F6 description, return addresses), `:16` (F4 thunk), `:55-56` (`kFnTeleport = 0x180a384,
kFnFire = 0x210c9c4`, `kTeleportRet = 0x210c22c, kFireRet = 0x210c2a4`), `:145` and `:164` (the two thunks), `:278-280` (the install and the
rollback). The core (`dronemuzzle_core.h`) takes its aim basis from AttackTarget's matrix (the game's code, see that header). The spawn rule and
its three constants are Ducky's reference combat build's (its drone fix F4), not verified in the game. The host test checks the core against
values worked out by hand (the aim along +Z with the cap, a slow aim, a zero, a NaN, a 1e-5 and a 2e-4 velocity) and against a trigonometric oracle (yaw and pitch, 200000 aims, every 97th vertical), to 1e-5 m.

(a source-build note, not in this tree):
```
| R49 | D3 WorldTeleport -> 0x190a384 (236) | CncaGameSpace::WorldTeleport | JUMP_SLOT 0x36cfa18 + GLOB_DAT 0x370bcc0 | 25 | ret 0x220c22c MATCH | GOT |
| R50 | D3 FireBullet -> 0x220c9c4 (976) | CR15NetBulletCS::FireBullet | JUMP_SLOT 0x36f3cf8 | 2 | ret 0x220c2a4 MATCH | GOT |
```
Disassembly (read this session, read-only: `functions`, `instructions`, `xrefs` tables):
```
R49 WorldTeleport | 190a384 | WorldTeleport | 236
R50 FireBullet | 220c9c4 | FireBullet | 976
== the two return sites inside FireBulletLocal: the bl and the return word ==
220c228 | bl | 0x00ff0660 | 0e91b797
220c22c | add | x0, sp, #0x178 | e0e30591
220c2a0 | bl | 0x01038c20 | 60b2b897
220c2a4 | ldr | x8, [x19] | 680240f9
== PLT stubs the two bl reach ==
ff0660 | adrp | x16, 0x37cf000 | f03e01f0        ldr x17, [x16, #0xa18]  -> slot 0x37cfa18 (ELF 0x36cfa18)  = R49's JUMP_SLOT
1038c20 | adrp | x16, 0x37f3000 | d03d01f0        ldr x17, [x16, #0xcf8]  -> slot 0x37f3cf8 (ELF 0x36f3cf8)  = R50's JUMP_SLOT
== xrefs to the WorldTeleport and FireBullet functions/slots ==
190a384 | call | 1      190a384 | data | 27      220c9c4 | call | 1      220c9c4 | data | 3
37cfa18 | read | 1      37f3cf8 | read | 1       380bcc0 | read | 15
```
So the return addresses (ELF = disassembly - 0x100000) 0x210c22c and 0x210c2a4 are the instructions after FireBulletLocal's two `bl`s (0x220c228
and 0x220c2a0), each going through the PLT stub of the row's JUMP_SLOT. The 15 GLOB_DAT reads of 0x380bcc0 (ELF 0x370bcc0) are **not** swapped
(the source does not either): a caller that reads the function address from the GOT entry and calls it bypasses the wrapper (those callers are
not FireBulletLocal's, which goes through the PLT).

## Behaviour on the host test and on arm64

`src/quest/tests/q15t/dronemuzzle_test.cpp`: the plain functions against fake real functions; the install against a fake slot page (real `mprotect`, real
`SafeRead`); the thunk decision through host stand-ins; the `#if defined(__aarch64__)` section of the test is empty on this host.

`src/quest/tests/q15t/mod/dronemuzzle.cmake` also cross-compiles the module with the NDK r26d clang (`-fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra
-Werror -O2`), checks that the object has no `.init_array` (the repack refuses a static constructor), and counts the instruction-word and relocation lines of the two
thunks (at least 38; `mod/dronemuzzle.cmake`). Output:
```
dronemuzzle: arm64 compile ok, no .init_array
dronemuzzle: arm64 thunks present (<n> lines of instruction words and relocations)
```

## Logging

One structured line per hook (INFO ok / ERROR refused), `dronemuzzle: on (QA)` with the return addresses, the muzzle constants, the back-off cap (`back_max_m`) and the shot
source (WARNING when dronekill's word is not swapped), `dronemuzzle: shot` per drone shot (the first 40: shot number, remote, root, spawn,
velocity, frame_ms, tid; then one `log cap reached` line, the counter goes on), `dronemuzzle: drone bullet tracer starts at the drone` (the first
3, then a cap line). No log line for a pass-through call (the hot path is a compare, a branch and a jump). No secrets.

## Prediction (written before any headset run)

With `Q15T_DRONEKILL=1` and `Q15T_DRONEMUZZLE=1` in an EC drone match on the pinned build:
1. Two `dronemuzzle hook install ... status=ok` lines and `dronemuzzle: on (QA) ... shot_source=dronekill FireBulletLocal word swapped` at INFO; no
   WARNING/ERROR from dronemuzzle.
2. Each drone shot produces one `dronemuzzle: shot` line (to 40) with `spawn` minus `root` equal to `kMuzzle` in the aim basis built from
   `velocity`, each to 1e-5 m: `kMuzzle[2]` (0.147 m) along the aim, less the back-off `min(|velocity| * frame_ms / 1000, 0.25)`; `kMuzzle[1]` (0.076 m) along the aim's Y (up for a level aim); `kMuzzle[0]` (0.000024 m) along X. The bullets visibly leave the drone's gun.
3. The first three bullets: `dronemuzzle: drone bullet tracer starts at the drone x= y= z=` with `x,y,z` equal to the same shot's `spawn`.
4. The player's own shots and every other WorldTeleport caller: no `dronemuzzle` line at all, and no change in behaviour or frame time.
5. Remote drone shots (`remote=1`) are moved the same way.

## Falsifiers

- `dronemuzzle: OFF, nothing changed` / `status=unexpected_original`: a slot does not hold its pinned function on this build.
- `dronemuzzle: on (QA) ... shot_source=NONE`: dronekill's FireBulletLocal word was not swapped; nothing will happen (by design).
- Zero `dronemuzzle: shot` lines although drones shoot (and dronekill's `fires` counter rises): the return address in the thunk does not match
  FireBulletLocal's call (the PLT bl is not the one read), or the slot is not what FireBulletLocal calls.
- `shot` lines present but bullets still leave the drone's middle: the pose copy is not the pose the bullet is spawned from.
- A `shot` line for a player shot (`velocity` = the player's aim): the accessor is non-null outside a drone shot.
- A crash in a caller of WorldTeleport/FireBullet other than FireBulletLocal: the thunk altered a register the caller needed (the asm
  text equals the source's, checked above; only x16 is used on the pass-through path).
- Bullets from the player's gun mis-placed: the record fix ran for a non-drone shot.
- `frame_ms` stays at 11.111111 (the 1/90 s default) for every `shot` of a run whose frames are not 90 Hz: the tick did not feed the
  running average (`DroneMuzzleTick` not registered, or never called).
- `spawn` minus `root` in a `shot` line differs from `kMuzzle` in the aim basis built from `velocity`, less `min(|velocity| * frame_ms / 1000, 0.25)` along the aim by more than 1e-5 m: the
  module does not compute the rule written here.

## Needs a word patch / not built

- **Needs a word patch: none.** Both rows are data-page GOT swaps (the thunk lives in the plugin's own code).
- **Not built:** the 15 GLOB_DAT reads of WorldTeleport's address (as in the source); file/property switches; the source's per-thread
  bookkeeping outside the shot accessor.
- **Unverified:** the return value widths: both thunks return `x0` as `uint64_t` (WorldTeleport's and FireBullet's return types were not
  read in the disassembly; FireBulletLocal ignores them as far as the source's use is concerned, the source build's dronemuzzle:145-182, unread beyond that); the bullet record
  offsets (+0x11c, +0x128, +0x134, stride 0x298, `bcs+0xc8/0xf8`) are the source's (copied, not re-derived); `CTransfQ` by invisible
  reference in x2 (AAPCS64 for a 32-byte composite; the source relies on the same).
- **Untestable on the host (documented, not mutated):** the order "publish the return and next words, then swap the slot" (a concurrency property:
  the host test can read the words only after `Install` returns); the shooter-position probe in F6 is covered by its two neighbours
  (the three fields are contiguous, so a page boundary cannot hide it): equivalent mutant. The `index == 0` / `records == 0` tests in F6 are
  equivalent to the `SafeRead` of a low address failing; the `v == nullptr` test is equivalent to `v2 == 0`.

## Open: whether a back-off belongs

The spawn rule moves the bullet back along the aim by `min(|v| dt, 0.25 m)` (`dronemuzzle_core.h`, `MuzzleMove`). The game's own bullet
update, `CR15NetBulletCS::UpdateMovement` (VA 0x2224290), already moves each bullet every frame, so whether the back-off is needed depends on
where that step falls in the frame. Read in the disassembly (libr15.so ELF vaddr + 0x100000):

- the frame delta is the float at +0x3c of the clock object (`ldr s0, [x8, #0x3c]`, 0x22242f4). The clock object is `[[space+0x10]]`,
  with space = `[this+0x80]`; `GlobalClock()` is called only when that field is 0 (UpdateMovement 0x22242cc..0x22242e0), through the
  thunk at 0xfd4860, a GOT jump to 0x1907720 (`ldr x0, [0x386cb30]`);
- the delta is scaled by the time scale (`fmul s0, s1, s0`, 0x2224414), the game's dt for the step;
- the bullet record's speed is at +0xcc (0x2224570), and travel is dt times speed (0x222457c);
- the step is `min(travel, rec+0xd0)` (`fsub` at 0x2224580, `cmp w8, #0` at 0x2224590, `fcsel lt` at 0x22245a0). FireBullet writes rec+0xd0 = -1.0 (0x220cce8). When it is negative, the step
  first raycasts (`bl 0xff7740` at 0x2224530) and then stores FLT_MAX or the hit distance (0x2224534..0x2224550); the negative test is
  at 0x22244c4..0x22244cc. The first-step hook point is for #2036;
- the direction is read from +0x104..+0x10c (0x22244e4..0x22244f0).

Not shown by this read: the order of UpdateMovement against the shot (FireBulletLocal's call to FireBullet) and against the first render of
the bullet in the same frame. If UpdateMovement runs first, the bullet is advanced once before it is drawn from the spawn; if it runs after,
the 0.25 m back-off is the only offset the first frame has. This module keeps main's clock on purpose: it does not read the game's clock, and dt is a running average of host frame intervals
(`FrameDt`, `ClampDt`). It could read the delta from the `gs` that `q15t_dm_teleport` receives (FireBulletLocal passes [this+0x80] at
0x220c21c), with no new address; switching is #2036's decision. The 0.25 m cap is the reference build's value (not read from the game). The question, and the evidence needed to answer it, is tracked in #2036.

## Tests and mutants

Host test (`just test-quest-q15t dronemuzzle`):
```
dronemuzzle_test: all checks passed
dronemuzzle: arm64 compile ok, no .init_array
dronemuzzle: arm64 thunks present (<n> lines of instruction words and relocations)
```
Mutants (`Q15T_MUTANTS_ONLY=dronemuzzle bash src/quest/tests/q15t/mutants.sh`): the rows are in `mutants.d/dronemuzzle.sh`: slots and return
constants, both thunk-word publishing checks, the all-or-none pre-check and rollback, the wrapper arguments, pose copy versus write, every F6
guard and probe, the caps, the on(QA) keys, and the core: the muzzle constants, the three terms of the offset, the basis, the length guard, the back-off (its cap and its frame time), the frame-time clamp and average, and the
root. The thunk-word ordering (publish, then swap) and the
shooter-position probe are not mutated (see above).

# dronemuzzle (rows R49 CncaGameSpace::WorldTeleport and R50 CR15NetBulletCS::FireBullet)

A combat drone's bullets leave its gun, not its root (drone fix F4 and F6). Two reviewed assembly entries through the hook manager; **no code page
is written**.

Files: `src/quest/combat/dronemuzzle.{h,cpp}`, `src/runtime/patch/dronemuzzle_rules.h` (the spawn rule, shared with PC's `src/runtime/combat/dronefix.cpp` and tested on PC by `src/runtime/tests/test_dronemuzzle_rules.cpp`), `dronemuzzle_targets.h` (the pins), `dronemuzzle_entry.inc`
(the two entries); tests `src/quest/tests/dronemuzzle_test.cpp` (`just test-quest-combat dronemuzzle`), `src/quest/combat/check/dronemuzzle.sh`
with `src/quest/tests/dronemuzzle_qemu.cpp`, the pins in `src/quest/tests/got_pinned_test.cpp`; mutants `src/quest/combat/mutants.d/dronemuzzle.sh`.

Origin: the reference build's `runtime/native/afkxr/afk_dronemuzzle.cpp` (its thunks at lines 145-182) and afk_dronemuzzle_core.h (kotorvr's
q15t-upstream), which port EchoCombat `c14c3e0a` `plugin/dronefix.cpp` rule for rule (the muzzle transform df 916-941, FireBullet df 943-1019,
the frame time df 132-147, MUZZLE df 51, BACK_MAX df 52).

## Switch and default

`combat.dronemuzzle` in `config.yaml`, a boolean, default **true**: the reference build ships `dronemuzzle.on` and the q15t plugin built the module
on. `false` installs nothing. The step installs only when dronekill's FireBulletLocal hook is installed (`dronekill::ShotActive`); otherwise it
refuses with `needs_dronekill_shot`. No switch file and no legacy file.

## What it hooks

| Row | Function | JUMP_SLOT (ELF) | real fn (ELF) = disassembly VA, size | acts on the call that returns to |
|---|---|---|---|---|
| R49 | `CncaGameSpace::WorldTeleport` | 0x36cfa18 (+ GLOB_DAT 0x370bcc0 not swapped) | 0x180a384 = 0x190a384, 236 | ELF 0x210c22c = disassembly 0x220c22c |
| R50 | `CR15NetBulletCS::FireBullet` | 0x36f3cf8 | 0x210c9c4 = 0x220c9c4, 976 | ELF 0x210c2a4 = disassembly 0x220c2a4 |

Both are called from `CR15NetAutoTargetCS::FireBulletLocal` (dronekill R48) among others (WorldTeleport has 25 callers, FireBullet 2), so the
hook must leave every other caller completely untouched: it needs the caller's return address as it is in x30, which a CallbackThunk's handler
cannot see (callback_thunk.h rule 7). So each slot holds a reviewed assembly entry (`dronemuzzle_entry.inc`, a top-level `asm()` with CFI, recorded
by `NEVR_ASM_ENTRY_RECORD`, installed by `InstallAsmEntry`):

```
bti c
adrp x16, <call-site word>; add x16, x16, :lo12:<word>; ldar x16, [x16]   ; FireBulletLocal's return address, published before the slot changes
cmp x30, x16 ; b.eq 1f
adrp x16, <next word>; add ...; ldar x16, [x16] ; br x16   ; every other caller: x0..x7, v0..v7, x30, sp as the caller left them (only x16)
1: stp x29, x30, [sp, #-16]! (CFI) ; mov x29, sp ; bl nevr_combat_dm_teleport / nevr_combat_dm_fire ; ldp x29, x30, [sp], #16 (CFI) ; ret
```
The next word is the manager's `nextOut`: the game's function, or the newest hook already on the slot, republished when that hook leaves. The
continuations load it with an acquire load at every call. The entry stays the newest hook on its slot: the manager refuses a later handler
above it (`slot_held_by_entry`), and it installs above a handler already there and calls it.

- **F4, `nevr_combat_dm_teleport`** (df 916-941): a drone shot (`dronekill::ShotVelocity` non-null) reads the 32-byte pose (`CTransfQ`, passed by
  invisible reference in x2) with `SafeRead`; the spawn = the pose's position (+0x10) plus the muzzle offset `(0.000024, 0.075634, 0.147283)`
  in the aim basis (Z = v/|v|, X = (z, 0, -x)/|..|, Y = Z x X; `dronemuzzle_rules.h`), then back along the aim by min(|v| dt, 0.25 m), dt the running average of host frame intervals (`FrameDt`, then `ClampDt`); the real function gets
  a **copy** with that position (the caller's pose is never written). |v| < 1e-4 or NaN, or an unreadable pose: the call goes through untouched.
- **F6, `nevr_combat_dm_fire`** (df 943-1019): the real FireBullet first; then for a drone shot with 1e-6 < v^2 < inf: the bullet record
  (`[bcs+0xf8] + ldrh([bcs+0xc8] + (handle & 0xffff) * 4) * 0x298`, the lookup FireBullet itself just made) gets shooter velocity (+0x11c) = 0 and
  shooter position (+0x128) = the record's spawn (+0x134), after readability probes on the three fields (a recycled handle or a short page:
  nothing is written). No effect on any other caller.

The step: the libr15 the sentinel attached (the pinned build ID), the frame tick (`tick::Add`), both call-site words published, then the WorldTeleport
entry and the FireBullet entry; a failed second install removes the first (`combat_dronemuzzle_rollback`). Each target names the pinned function
as the expected original (the chain's oldest original when another hook is there first).

## Arena (same APK)

The continuation acts only inside a drone shot, and dronekill makes no shot frame outside a combat map (dronekill.md, "Arena"), so in an arena
level nothing is moved or written: a call from FireBulletLocal's call site (none in arena: no drone fires) would reach the continuation, which
calls the next function unchanged. Every other WorldTeleport and FireBullet call is a non-drone call.

### Arena cost (ADR 0038)

A non-drone call (every WorldTeleport and FireBullet call in an arena level) runs ten instructions of the entry before the game's function, counted
in the arm64 object and pinned by `check/dronemuzzle.sh`: `bti c`; `adrp`, `add`, `ldar` (the call-site word); `cmp x30, x16`; `b.eq` (not taken);
`adrp`, `add`, `ldar` (the next word); `br x16`. No call, no stack, no lock. How often: WorldTeleport has 25 callers in libr15 (respawns, teleports;
a headset count is not measured), FireBullet 2. Out of dronekill's scope the frame tick reads no clock: 13 instructions and `dronekill::InCombatScope` (4), per frame; in scope it reads the
clock (`clock_gettime`) and updates a float average.


## Lines (sentinel log)

- `combat_dronemuzzle_hook` `{"hook":"<name>","status":"installed|<GotStatus>"}`: Info when installed, Error otherwise; the manager's `got_hook` line.
- `combat_dronemuzzle_install`: `"result":"installed"` with the two call sites, the muzzle constants and `back_max_m`; `"result":"off"`; `"result":"refused"`
  with `reason` and the consequence (Error).
- `combat_dronemuzzle_shot` (the first 40: shot number, remote, root, spawn, velocity, frame_ms, tid) and `combat_dronemuzzle_tracer` (the first
  3), each with one `combat_dronemuzzle_cap` line at its cap; the counters go on. No line for a pass-through call. No secret.

## Evidence

Source build (read-only): the source build's dronemuzzle:7-10 (F4/F6 description, return addresses), `:16` (F4 thunk), `:55-56` (`kFnTeleport = 0x180a384,
kFnFire = 0x210c9c4`, `kTeleportRet = 0x210c22c, kFireRet = 0x210c2a4`), `:145` and `:164` (the two thunks), `:278-280` (the install and the
rollback). The core (`dronemuzzle_rules.h`) takes its aim basis from AttackTarget's matrix (the game's code, see that header). The spawn rule and
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

## Headset row (a prediction: no headset run in this unit)

With `combat.dronekill` and `combat.dronemuzzle` at their defaults (true) in an EC drone match on the combat APK:
1. Two `combat_dronemuzzle_hook` lines with `"status":"installed"` and `combat_dronemuzzle_install` `"result":"installed"` at Info; no Warning or
   Error from dronemuzzle; the `combat_install_module` record for dronemuzzle has `"outcome":"installed"`.
2. Each drone shot produces one `combat_dronemuzzle_shot` line (to 40) with `spawn` minus `root` equal to `kMuzzle` in the aim basis built from
   `velocity`, each to 1e-5 m: `kMuzzle[2]` (0.147 m) along the aim, less the back-off `min(|velocity| * frame_ms / 1000, 0.25)`; `kMuzzle[1]` (0.076 m) along the aim's Y (up for a level aim); `kMuzzle[0]` (0.000024 m) along X. The bullets visibly leave the drone's gun.
3. The first three bullets: `combat_dronemuzzle_tracer` with `x,y,z` equal to the same shot's `spawn`.
4. The player's own shots and every other WorldTeleport caller: no `combat_dronemuzzle_*` line at all, and no change in behaviour or frame time.
5. Remote drone shots (`"remote":1`) are moved the same way.

6. An arena match on the combat APK: no `combat_dronemuzzle_*` line after the install lines, and no change anyone can see.

## Falsifiers

- `combat_dronemuzzle_install` `"result":"refused"` with a hook `"status":"original_mismatch"`: a slot does not hold its pinned function on this build.
- `combat_dronemuzzle_install` `"reason":"needs_dronekill_shot"`: dronekill's FireBulletLocal hook is not installed; nothing is installed (by design).
- Zero `combat_dronemuzzle_shot` lines although drones shoot (and dronekill's `fires` counter rises): the return address in the entry does not match
  FireBulletLocal's call (the PLT bl is not the one read), or the slot is not what FireBulletLocal calls.
- `shot` lines present but bullets still leave the drone's middle: the pose copy is not the pose the bullet is spawned from.
- A `shot` line for a player shot (`velocity` = the player's aim): the accessor is non-null outside a drone shot.
- A crash in a caller of WorldTeleport/FireBullet other than FireBulletLocal: the entry altered a register the caller needed (check/dronemuzzle.sh pins the entry's
  text; only x16 is used on the pass-through path).
- Bullets from the player's gun mis-placed: the record fix ran for a non-drone shot.
- `frame_ms` stays at 11.111111 (the 1/90 s default) for every `shot` of a run whose frames are not 90 Hz: the tick did not feed the
  running average (`dronemuzzle::Tick` not registered, or never called).
- `spawn` minus `root` in a `shot` line differs from `kMuzzle` in the aim basis built from `velocity`, less `min(|velocity| * frame_ms / 1000, 0.25)` along the aim by more than 1e-5 m: the
  module does not compute the rule written here.

## Needs a word patch / not built

- **Needs a word patch: none.** Both rows are JUMP_SLOT hooks through the manager (the entries live in the sentinel's code).
- **Not built:** the 15 GLOB_DAT reads of WorldTeleport's address (as in the source); file or property switches; the reference build's per-thread
  bookkeeping outside the shot accessor.
- **Unverified:** the return value widths: both entries return `x0` as `uint64_t` (WorldTeleport's and FireBullet's return types were not
  read in the disassembly; FireBulletLocal ignores them as far as the source's use is concerned, the source build's dronemuzzle:145-182, unread beyond that); the bullet record
  offsets (+0x11c, +0x128, +0x134, stride 0x298, `bcs+0xc8/0xf8`) are the source's (copied, not re-derived); `CTransfQ` by invisible
  reference in x2 (AAPCS64 for a 32-byte composite; the source relies on the same).
- **Untestable on the host (documented, not mutated):** the order "publish the call-site and next words, then change the slot" (a concurrency property:
  the host test can read the words only after `Install` returns); the shooter-position probe in F6 is covered by its two neighbours
  (the three fields are contiguous, so a page boundary cannot hide it): equivalent mutant. The `index == 0` / `records == 0` tests in F6 are
  equivalent to the `SafeRead` of a low address failing; the `v == nullptr` test is equivalent to `v2 == 0`.

## Open: whether a back-off belongs

The spawn rule moves the bullet back along the aim by `min(|v| dt, 0.25 m)` (`dronemuzzle_rules.h`, `MuzzleMove`). The game's own bullet
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
(`FrameDt`, `ClampDt`). It could read the delta from the `gs` that `nevr_combat_dm_teleport` receives (FireBulletLocal passes [this+0x80] at
0x220c21c), with no new address; switching is #2036's decision. The 0.25 m cap is the reference build's value (not read from the game). The question, and the evidence needed to answer it, is tracked in #2036.

## Tests and mutants

`dronemuzzle_test` (the `combat` host-test group): the core against a trigonometric oracle (200000 aims) and hand values, the install of both
entries through the real GotHook into a libr15 image built in memory (on the host the entries are dronemuzzle.cpp's C++ stand-ins: the same
return-address decision), the continuations' rules, the frame tick, another handler on the slot in both orders, the step's refusals.
`check/dronemuzzle.sh` (run by `just test-quest-combat dronemuzzle`): the two entries as NDK clang compiles `dronemuzzle.cpp` for arm64 are exactly the
expected instructions (both words by `ldar`, the pass-through a `br x16`); every `blr`/`br` of each continuation goes through a register only an
`ldar` of the next word sets on its path; `dronemuzzle_qemu` runs the entries under qemu-aarch64 (the acting call reaches the continuation with
x0..x6 and s0 intact, every other call reaches the next word, the caller's callee-saved registers survive). The frame sensor (`tests/quest`)
walks the two records on the built sentinel. Mutants: `src/quest/combat/mutants.sh dronemuzzle`; the entry text's mutants are the check script's.


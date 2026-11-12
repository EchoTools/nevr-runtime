# Module `bullet_velocity_guard` (#1737, #1651)

Files: `src/quest/combat/bullet_velocity_guard.{h,cpp}`, `src/quest/combat/bullet_velocity_guard_targets.h` (the pins),
`src/runtime/combat/bullet_velocity_guard_rules.h` (the rule, one definition for PC and Quest), `src/quest/tests/bullet_velocity_guard_test.cpp`,
`src/quest/tests/bullet_velocity_guard_pinned_test.cpp`, `src/quest/combat/mutants.d/bullet_velocity_guard.sh`. A normal-phase row of the combat
install table (`src/quest/combat/combat_install.h`), immediately above `dronemuzzle`'s: it runs in the combat APKs and in no other.

## Origin

The fireguard switch of kotorvr's echoquest-combat: runtime/native/afkxr/afk_fx.cpp, its `HookFire` hook and the "guards" block (the switch's hook and block are named in `bullet_velocity_guard_targets.h`, not by line), read as text. What is taken is the behaviour (a FireBullet call with a near-zero or non-finite velocity is
not made). The mechanism, the threshold and the lines are this tree's: a `CallbackThunk` through the hook manager, and the squared-length rule
dronemuzzle already applied to drone shots.

## Switch and default

`combat.bullet_velocity_guard` in `config.yaml`, a boolean, default **true**. A value that is not a boolean keeps the default, with a Warning that
names the key.

This departs from upstream on purpose. The recipe this port was planned from ships its fireguard off (`fxfireguard.on` is not among its switch
files), while the working Quest combat build #1651 was measured on has it on. It is on here because it passes every call that does not divide by
zero unchanged, and because the tree already applies the same rule to drone shots inside dronemuzzle (its continuation leaves a drone shot's record
alone on the same condition): one rule, now applied at the one place every shot passes.

## What it hooks

`NRadEngine::NRadGame::CR15NetBulletCS::FireBullet`, JUMP_SLOT ELF 0x36f3cf8 -> 0x210c9c4 (dronemuzzle's pin, `dronemuzzle_targets.h`, not
repinned). FireBullet normalises its velocity argument with no test: `fmov s3, #1.0` at 0x210ccac, `fsqrt s4, s4` at 0x210ccc0, `fdiv s3, s3, s4`
at 0x210ccc4 (the three words of `bullet_velocity_guard_targets.h`, which the install checks before it touches the slot and
`bullet_velocity_guard_pinned_test` checks against the store file). A zero vector gives 1/0 and a NaN direction; a non-finite component gives a
non-finite one.

The velocity is the fifth argument (x4, `const C3Vector&`). At FireBulletLocal's call (ELF 0x210c2a0, VA 0x220c2a0) x4 is the address of
FireBulletLocal's own by-value velocity parameter (`C3Vector dir`, arriving in s0..s2 and spilled to its stack): `&dir`, with the bullet's handle
in w1, the third argument in x2 (below), the transform in x3, two zero words in w5 and w6 and the damage float in s0 (below). The
reconstruction is `CR15NetAutoTargetCS::FireBulletLocal`, VA 0x220bff8. FireBulletLocal reads `dir` and never writes it between its entry and
that call. x2 is a `CComponentHandle<CR15NetGunCS>` passed by value: the mangled name of FireBullet in `dronemuzzle_targets.h` has no reference mark, and
the header (`bullet_velocity_guard.h`) says the same. The object is 0x50 bytes (nevr-giveyouup `fire_bullet_local.cpp:90`, the proven tree's `CComponentHandle` size).
The proven tree's caller (nevr-giveyouup, `src/reconstruction/r15/fire_bullet_local.cpp:275-279`) fills the object it passes with a copy of the
weapon record, so what its fields hold is not established. The guard does not read x2.
s0 is the damage float, which that caller loads from the weapon record at +0x70 (nevr-giveyouup `fire_bullet_local.cpp:280-281`).

The guard and dronemuzzle see the same vector. dronemuzzle's continuation acts only on that one call (its entry compares the return address with
FireBulletLocal's, ELF 0x210c2a4), and skips on dronekill's shot velocity, which dronekill copies from the same by-value parameter on entry to
FireBulletLocal and passes on unchanged (`FireInScope` in `src/quest/combat/dronekill.cpp`: `ShotFrame frame{velocity, remote}`, then
`real(cs, handle, velocity, remote)`). So when the guard skips FireBullet on that call, the continuation's own test skips too, and it never writes
to a record FireBullet did not fill. A future hook that changed the velocity between FireBulletLocal's entry and its FireBullet call would break
that agreement; none exists in this tree.

The rule (`nevr_combat::bullet_velocity_guard::Skips`): skip when the vector is null, when x² + y² + z² is not above 1e-6f (|v| at most about 1e-3 m/s: the float 1e-3f squared is just above 1e-6f, so a speed of exactly 1e-3f passes), or when
that sum is not finite (a NaN or infinite component, or an overflow). A skipped call returns 0 without calling FireBullet; every other call reaches
FireBullet with the same eight arguments and its result is returned.

Order on the slot. dronemuzzle's assembly entry sits on the same slot and must stay the newest hook there: the manager refuses any install above it
(`slot_held_by_entry`). So the guard's row is immediately above dronemuzzle's: the guard installs first, the entry joins above it and calls it as its
next word. A shot the guard skips is skipped for dronemuzzle's continuation as well, which already does nothing on the same condition.

## Log lines (sentinel log)

- `combat_bullet_velocity_guard_install`: `result` (`installed|off|refused`), `reason` (`ok`, `off_config`, `module_not_loaded`, `no_build_id`,
  `build_id_mismatch`, `pinned_word_mismatch`, or the manager's status by name: `original_mismatch`, `slot_held_by_entry`, ...), `slot`, `msg`. Info
  for installed and off, Error for a refusal; `pinned_word_mismatch` adds `pin`, `vaddr`, `found` (`unreadable` for a page that cannot be read) and
  `expected`.
- `combat_bullet_velocity_guard_skip`: Info, the first 10 skips of the process: `n`, `vx`, `vy`, `vz` (or `vel` `null`), `index` (the bullet
  handle's low 16 bits), `msg` "a shot with no usable velocity was not fired: FireBullet would divide by zero and store a NaN direction".
- `combat_bullet_velocity_guard_skips`: Info, once at the 11th skip with `since_last` 0 (later ones are counted, not logged), then at most every
  10 s while skips continue, with `n` and `since_last` (`nevr::BurstThenCount`, `src/core/burst_then_count.h`).
- `combat_bullet_velocity_guard_total`: Info at the end of the process, only when installed: `skipped`, `calls`.

## Arena cost and arena behaviour (arena and combat share the APK)

- **Behaviour.** The guard acts in every level: README rule 9's crash-only exception. The divide is in code arena and combat share, and a level
  answer that lags a level change would leave it in place. Every call that does not take the divide-by-zero path reaches FireBullet with the same
  arguments and answers what FireBullet answers (`bullet_velocity_guard_test`, `PassCallsOriginalUnchanged`), so an arena match sees the game's own
  function. Whether a NaN direction faults, or only makes a bullet that goes nowhere, is not shown (below): the exception rests on the unguarded
  divide, not on a measured crash.
- **Cost.** FireBullet runs once per shot (FireBulletLocal and one other caller), not per frame. Per call: the thunk's entry, one relaxed atomic
  increment, three multiplies, two adds and two compares. A skip adds a lock around the line budget and, for the first ten, one line. Not measured
  on a headset.

## Not shown

- What becomes of a skipped shot's bullet. FireBulletLocal has already allocated a pooled bullet actor, teleported it to the spawn pose and set its
  owner before it calls FireBullet, and after it returns it sends the fire event and broadcasts the shot whatever FireBullet did (the
  reconstruction of FireBulletLocal, 0x220c11c..0x220c484). Whether that actor stays visible, is reclaimed, or is reused is not established:
  FireBullet itself (VA 0x220c9c4) is not yet in the reconstruction (requested for this unit), and the headset row below looks for it.
- That a zero or non-finite velocity reaches FireBullet in play on the store base, and that a NaN direction faults.
- That FireBullet's other caller passes a velocity at x4 the same way (only FireBulletLocal's call was read).

## Headset row

Quest 3, combat APK, two combat matches: `combat_bullet_velocity_guard_install` `installed` at start; any `combat_bullet_velocity_guard_skip` lines
with their vectors; no stall after a first shot; `combat_bullet_velocity_guard_total` at the end with `calls` > 0. If a skip occurs, note whether a
bullet actor stays visible where the shot would have started.

## Platform

PC: not built here. echovr.exe's FireBullet (VA 0x140ce2020) has the same unguarded divide (SQRTSS at 0x140ce2442, DIVSS at 0x140ce244e), but which
argument carries the velocity there, and whether a zero vector ever reaches it, are not established. #2375 tracks the PC guard, which reuses
`bullet_velocity_guard_rules.h`.

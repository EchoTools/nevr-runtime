// The two assembly entries of dronemuzzle (src/quest/combat/dronemuzzle.h). Included, at global scope, by dronemuzzle.cpp and by
// src/quest/tests/dronemuzzle_qemu.cpp (the freestanding qemu test), so the test runs the very text the sentinel builds.
//
// Each entry compares the caller's return address (x30) with its call-site word: a call that returns into FireBulletLocal gets a frame, a `bl` to
// the continuation and a return; every other call jumps (`br x16`) to the next word with x0..x7, v0..v7, x30 and sp as the caller left them (x16
// only). Both words are read with an acquire load (`ldar`) at every call (callback_thunk.h rule 7, #2216). `hint #34` is `bti c`. The CFI
// describes the one frame the acting path pushes; the pass-through path runs under the default row (CFA = sp, return address in x30).
//   nevr_combat_dm_tp_entry    CncaGameSpace::WorldTeleport(gs, actor, pose, reason): x0..x3 reach nevr_combat_dm_teleport as the game passed them
//   nevr_combat_dm_fire_entry  CR15NetBulletCS::FireBullet(bcs, handle, gun, damage, quat, vel, remote, flag): x0..x6 and s0 reach nevr_combat_dm_fire
asm(R"(
  .text
  .p2align 2
  .hidden nevr_combat_dm_tp_entry
  .type nevr_combat_dm_tp_entry,%function
nevr_combat_dm_tp_entry:
  .cfi_startproc
  hint #34
  adrp x16, nevr_combat_dm_tp_ret
  add x16, x16, :lo12:nevr_combat_dm_tp_ret
  ldar x16, [x16]
  cmp x30, x16
  b.eq 1f
  adrp x16, nevr_combat_dm_tp_next
  add x16, x16, :lo12:nevr_combat_dm_tp_next
  ldar x16, [x16]
  br x16
1:
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset 29, -16
  .cfi_offset 30, -8
  mov x29, sp
  bl nevr_combat_dm_teleport
  ldp x29, x30, [sp], #16
  .cfi_restore 29
  .cfi_restore 30
  .cfi_def_cfa_offset 0
  ret
  .cfi_endproc
  .size nevr_combat_dm_tp_entry, .-nevr_combat_dm_tp_entry

  .p2align 2
  .hidden nevr_combat_dm_fire_entry
  .type nevr_combat_dm_fire_entry,%function
nevr_combat_dm_fire_entry:
  .cfi_startproc
  hint #34
  adrp x16, nevr_combat_dm_fire_ret
  add x16, x16, :lo12:nevr_combat_dm_fire_ret
  ldar x16, [x16]
  cmp x30, x16
  b.eq 1f
  adrp x16, nevr_combat_dm_fire_next
  add x16, x16, :lo12:nevr_combat_dm_fire_next
  ldar x16, [x16]
  br x16
1:
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset 29, -16
  .cfi_offset 30, -8
  mov x29, sp
  bl nevr_combat_dm_fire
  ldp x29, x30, [sp], #16
  .cfi_restore 29
  .cfi_restore 30
  .cfi_def_cfa_offset 0
  ret
  .cfi_endproc
  .size nevr_combat_dm_fire_entry, .-nevr_combat_dm_fire_entry
)");

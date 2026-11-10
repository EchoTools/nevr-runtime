// POWER BOOST's assembly entry on CPhTarget::ResolveBody's JUMP_SLOT (src/quest/combat/powerboost.h, callback_thunk.h rule 7). Included, at
// global scope, by src/quest/combat/powerboost.cpp and by src/quest/tests/pb_thunk_qemu.cpp (the freestanding qemu test), so the test runs the
// very text the sentinel builds. No code page is written: the only write is the manager's store of this entry into the slot.
//
// ResolveBody has 771 callers. Every call but one goes straight on to the next word: x16 is the only register the entry uses, no stack is
// touched, x0..x7, x8, x30 and sp reach the next function as the caller left them, so it returns to the caller itself. The one call is the
// `bl` at ELF 0x2157998 (UpdateActiveHealBubble's per-target call), recognised by its return address x30 == nevr_combat_pb_cone_ret
// (base + 0x215799c). That call gets a frame and runs nevr_combat_pb_cone_at(x0 = the CPhTarget*, x1 = UpdateActiveHealBubble's sp, x2 = its
// x24 = the target's CR15NetDamageableCS*) and returns what it returns. x24 is callee-saved, so it still holds the value UpdateActiveHealBubble
// loaded at ELF 0x215794c.
//
// Both words are read with an acquire load (`ldar`) at every call (#2216): the call-site word, which the install stores (release) before the
// slot changes, and the next word, which the manager publishes (release) before the slot changes and again when the hook it calls leaves
// the chain. `hint #34` is `bti c`. The CFI describes the frame of the acting path; the pass-through path has the default row (CFA = sp,
// return address in x30).
asm(R"(
  .text
  .p2align 2
  .hidden nevr_combat_pb_resolve_entry
  .type nevr_combat_pb_resolve_entry,%function
nevr_combat_pb_resolve_entry:
  .cfi_startproc
  hint #34
  adrp x16, nevr_combat_pb_cone_ret
  add x16, x16, :lo12:nevr_combat_pb_cone_ret
  ldar x16, [x16]
  cmp x30, x16
  b.eq 1f
  adrp x16, nevr_combat_pb_next
  add x16, x16, :lo12:nevr_combat_pb_next
  ldar x16, [x16]
  br x16
1:
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset 29, -16
  .cfi_offset 30, -8
  mov x29, sp
  add x1, sp, #16
  mov x2, x24
  bl nevr_combat_pb_cone_at
  ldp x29, x30, [sp], #16
  .cfi_restore 29
  .cfi_restore 30
  .cfi_def_cfa_offset 0
  ret
  .cfi_endproc
  .size nevr_combat_pb_resolve_entry, .-nevr_combat_pb_resolve_entry
)");

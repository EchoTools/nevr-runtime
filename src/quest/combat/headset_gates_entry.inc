// The two assembly entries of the headset gates (src/quest/combat/headset_gates.h). Included, at global scope, by headset_gates.cpp and by
// src/quest/tests/headset_gates_qemu.cpp (the freestanding qemu test), so the test runs the very text the sentinel builds.
//
// Each entry is entered by the game's call and acts on every call, so it has no pass-through path: it takes what the continuation needs (a jump
// touches neither x30 nor sp), then tail-branches (`b`) with x30 and sp as the caller left them, so the continuation returns straight to the
// caller. `hint #34` is `bti c`. A top-level block has exactly the CFI it writes (callback_thunk.h rule 7): the entry builds no frame, so the
// default row (CFA = sp, return address in x30) holds for all of it, and `.cfi_startproc`/`.cfi_endproc` give it its FDE.
//   nevr_combat_headset_tramp  CVR::HeadsetType(): x0 = the caller's sp, x1 = the caller's return address; the Bind reads its own caller at sp+0x18.
//   nevr_combat_party_tramp    PartyMemberHeadsetType(this, index): x0, x1 as the game passed them, x2 = the caller's return address.
asm(R"(
  .text
  .p2align 2
  .hidden nevr_combat_headset_tramp
  .type nevr_combat_headset_tramp,%function
nevr_combat_headset_tramp:
  .cfi_startproc
  hint #34
  mov x0, sp
  mov x1, x30
  b nevr_combat_headset_dispatch
  .cfi_endproc
  .size nevr_combat_headset_tramp, .-nevr_combat_headset_tramp

  .p2align 2
  .hidden nevr_combat_party_tramp
  .type nevr_combat_party_tramp,%function
nevr_combat_party_tramp:
  .cfi_startproc
  hint #34
  mov x2, x30
  b nevr_combat_party_dispatch
  .cfi_endproc
  .size nevr_combat_party_tramp, .-nevr_combat_party_tramp
)");

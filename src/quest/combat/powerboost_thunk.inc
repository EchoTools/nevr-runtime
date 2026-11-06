// The one naked arm64 function of the plugin (source build: the source build's powerboost "PB-1", source-build pb_resolve_thunk).
// Included, inside `extern "C" {`, by src/quest/plugins/q15t/src/powerboost.cpp and by src/quest/tests/q15t/pb_thunk_qemu.cpp (the freestanding qemu test),
// so the test runs the very text the plugin builds. No code page is written: this function is ordinary text of the
// plugin; the only write is the data-page GOT swap that points CPhTarget::ResolveBody's JUMP_SLOT at it.
//
// ResolveBody has 771 callers (R55). Any caller but one goes straight on to the real function: x16 is the only register
// used, no stack is touched, x0 and x30 arrive at the real function unchanged, so it returns to the caller itself.
// The one caller is the `bl` at ELF 0x2157998 (UpdateActiveHealBubble's per-target call), recognised by its return
// address x30 == q15t_pb_cone_ret (= base + 0x215799c). That call runs q15t_pb_cone_at(x0 = the CPhTarget*,
// x1 = UpdateActiveHealBubble's sp, x2 = its x24 = the target's CR15NetDamageableCS*) and returns what it returns.
// x24 is callee-saved, so it still holds the value UpdateActiveHealBubble loaded at ELF 0x215794c.
// Both words are stored (release) before the one slot store and never again.
__attribute__((naked, visibility("hidden"))) void q15t_pb_resolve_thunk() {
  asm volatile(
      "hint #34\n"  // bti c
      "adrp x16, q15t_pb_cone_ret\n"
      "ldr x16, [x16, :lo12:q15t_pb_cone_ret]\n"
      "cmp x30, x16\n"
      "b.eq 1f\n"
      "adrp x16, q15t_pb_resolve_real\n"
      "ldr x16, [x16, :lo12:q15t_pb_resolve_real]\n"
      "br x16\n"
      "1:\n"
      "stp x29, x30, [sp, #-16]!\n"
      "mov x29, sp\n"
      "add x1, sp, #16\n"  // UpdateActiveHealBubble's sp
      "mov x2, x24\n"      // its target CR15NetDamageableCS*
      "bl q15t_pb_cone_at\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}

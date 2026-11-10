// NDK clang -O2 -funwind-tables output for: int k(int x) { int r = g(x); if (r) return r; return g(x + 1) + 2; }. The
// sensor MUST pass it.
#include "frames_asm_probe_entry.h"
asm(R"(
.text
.p2align 2
.hidden probe_asm_entry
.type probe_asm_entry,%function
probe_asm_entry:
.cfi_startproc
stp x29, x30, [sp, #-32]!
.cfi_def_cfa_offset 32
str x19, [sp, #16]
mov x29, sp
.cfi_def_cfa w29, 32
.cfi_offset w19, -16
.cfi_offset w30, -24
.cfi_offset w29, -32
mov w19, w0
bl probe_asm_continuation
cbnz w0, 2f
add w0, w19, #1
bl probe_asm_continuation
add w0, w0, #2
2:
.cfi_def_cfa wsp, 32
ldr x19, [sp, #16]
ldp x29, x30, [sp], #32
.cfi_def_cfa_offset 0
.cfi_restore w19
.cfi_restore w30
.cfi_restore w29
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

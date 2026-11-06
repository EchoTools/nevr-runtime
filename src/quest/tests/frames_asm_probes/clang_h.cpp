// NDK clang -O2 -funwind-tables output for: int h(int x) { if (x) return g(x) * 3; return 7; }. The sensor MUST pass it.
#include "frames_asm_probe_entry.h"
asm(R"(
.text
.p2align 2
.hidden probe_asm_entry
.type probe_asm_entry,%function
probe_asm_entry:
.cfi_startproc
cbz w0, 2f
stp x29, x30, [sp, #-16]!
.cfi_def_cfa_offset 16
mov x29, sp
.cfi_def_cfa w29, 16
.cfi_offset w30, -8
.cfi_offset w29, -16
bl probe_asm_continuation
add w0, w0, w0, lsl #1
.cfi_def_cfa wsp, 16
ldp x29, x30, [sp], #16
.cfi_def_cfa_offset 0
.cfi_restore w30
.cfi_restore w29
ret
2:
mov w0, #7
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

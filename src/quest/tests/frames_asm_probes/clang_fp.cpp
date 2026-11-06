// Clang's shape for a frame with locals: sub sp 32, stp at [sp, #16], add x29, CFA on x29, CFA back on sp before the pop.
// The sensor MUST pass it.
#include "frames_asm_probe_entry.h"
asm(R"(
.text
.p2align 2
.hidden probe_asm_entry
.type probe_asm_entry,%function
probe_asm_entry:
.cfi_startproc
hint #34
adrp x16, probe_asm_ret
add x16, x16, :lo12:probe_asm_ret
ldar x16, [x16]
cmp x30, x16
b.eq 1f
adrp x16, probe_asm_next
add x16, x16, :lo12:probe_asm_next
ldar x16, [x16]
br x16
1:
sub sp, sp, #32
.cfi_def_cfa_offset 32
stp x29, x30, [sp, #16]
add x29, sp, #16
.cfi_def_cfa w29, 16
.cfi_offset w30, -8
.cfi_offset w29, -16
bl probe_asm_continuation
.cfi_def_cfa wsp, 32
ldp x29, x30, [sp, #16]
add sp, sp, #32
.cfi_def_cfa_offset 0
.cfi_restore w30
.cfi_restore w29
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

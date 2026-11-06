// X30 saved at CFA-24, then stp x0, x1, [sp, #8] overwrites that slot with x0; the pop reloads x0 into x30. The sensor MUST
// fail it.
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
stp x29, x30, [sp, #-32]!
.cfi_def_cfa_offset 32
.cfi_offset 29, -32
.cfi_offset 30, -24
mov x29, sp
stp x0, x1, [sp, #8]
bl probe_asm_continuation
ldp x29, x30, [sp], #32
.cfi_restore 29
.cfi_restore 30
.cfi_def_cfa_offset 0
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

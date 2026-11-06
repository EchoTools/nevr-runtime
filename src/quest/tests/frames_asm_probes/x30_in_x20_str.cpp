// X30 kept in x20, x20 saved alone at CFA-16 with str pre-index. The sensor MUST pass it.
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
str x20, [sp, #-16]!
.cfi_def_cfa_offset 16
.cfi_offset 20, -16
mov x20, x30
.cfi_register 30, 20
bl probe_asm_continuation
mov x30, x20
.cfi_restore 30
ldr x20, [sp], #16
.cfi_restore 20
.cfi_def_cfa_offset 0
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

// The pop reloads x30 and moves sp past the slot; sp is moved down again without a store, and the second bl relies on the
// slot below the old sp (a signal frame may have overwritten it). The sensor MUST fail it.
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
stp x29, x30, [sp, #-16]!
.cfi_def_cfa_offset 16
.cfi_offset 29, -16
.cfi_offset 30, -8
mov x29, sp
bl probe_asm_continuation
ldp x29, x30, [sp], #16
.cfi_restore 29
.cfi_restore 30
.cfi_def_cfa_offset 0
sub sp, sp, #16
.cfi_def_cfa_offset 16
.cfi_offset 30, -8
bl probe_asm_continuation
ldr x30, [sp, #8]
add sp, sp, #16
.cfi_restore 30
.cfi_def_cfa_offset 0
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

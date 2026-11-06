// The unwind rows are right at the bl and wrong everywhere else: between the stp and the bl the row says CFA=sp+0,
// and after the ldp it keeps CFA=sp+16. The sensor MUST fail it.
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
mov x29, sp
.cfi_def_cfa_offset 16
.cfi_offset 29, -16
.cfi_offset 30, -8
bl probe_asm_continuation
ldp x29, x30, [sp], #16
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

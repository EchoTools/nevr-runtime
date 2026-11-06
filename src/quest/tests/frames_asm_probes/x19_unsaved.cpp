// X30 kept in x19 with .cfi_register, but the caller's x19 is never saved: the entry returns with the caller's x19
// replaced. The sensor MUST fail it.
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
mov x19, x30
.cfi_register 30, 19
bl probe_asm_continuation
mov x30, x19
.cfi_restore 30
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

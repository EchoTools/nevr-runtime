// x30 is stored below sp (sp is not moved): the continuation's own prologue overwrites the slot before it returns.
// The sensor MUST fail it.
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
stur x30, [sp, #-8]
.cfi_offset 30, -8
bl probe_asm_continuation
ldur x30, [sp, #-8]
.cfi_restore 30
ret
.cfi_endproc
.size probe_asm_entry, .-probe_asm_entry
)");

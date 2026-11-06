// Built -fno-exceptions. A reviewed assembly entry (sentinel/callback_thunk.h rule 7) whose continuation calls a helper with a
// personality directly, across the call into the "game": the frame sensor's controls for the entry kind
// (tests/quest frames_sensor_test.go). Never run: the sensor reads the linked executable.
//
//   frames_asm_probe_ok      a top-level asm() entry whose CFI describes its frame, with ldar loads; the helper is annotated
//                            NEVR_OUTSIDE_GAME_CALL: the sensor MUST pass it
//   frames_asm_probe_bad     the helper is NOT annotated: it is live across the call, so the sensor MUST fail it
//   frames_asm_probe_nocfi   the entry has no CFI, so no FDE and a game exception cannot unwind through it: the sensor MUST fail it
//   frames_asm_probe_naked   the same body as an __attribute__((naked)) function with no CFI (the shape of the q15t entries):
//                            clang gives it an FDE whose one row is CFA=sp, wrong after the stp and at the bl: the sensor MUST
//                            fail it
//   frames_asm_probe_ldr     the two words are read with a plain ldr (#2216's shape): the sensor MUST fail it
//
// The entry has the shape of the plugin's (dronemuzzle): act only when x30 is the game's call site, else jump to the next word
// with the caller's registers untouched.
#include "hook_install.h"

namespace probe {
// Defined in frames_probe_helper.inc (exceptions enabled): a try/catch, so its CIE names a personality.
int LiveHelper(int value) noexcept;
}  // namespace probe

extern "C" {
int probe_asm_entry(int value);
void* probe_asm_ret = nullptr;   // the game's call site (the return address the entry acts on)
void* probe_asm_next = nullptr;  // what the entry jumps to for every other call; the manager publishes it
int probe_asm_continuation(int value) noexcept { return probe::LiveHelper(value); }
}

#if defined(PROBE_NO_CFI) || defined(PROBE_NAKED)
#define PROBE_CFI(directive) ""
#else
#define PROBE_CFI(directive) directive "\n"
#endif

#if defined(PROBE_PLAIN_LDR)
#define PROBE_LOAD(word) "adrp x16, " word "\nldr x16, [x16, :lo12:" word "]\n"
#else
#define PROBE_LOAD(word) "adrp x16, " word "\nadd x16, x16, :lo12:" word "\nldar x16, [x16]\n"
#endif

// The entry's text, without the function's own start and end.
#define PROBE_ENTRY_BODY                                                                                     \
  "hint #34\n" PROBE_LOAD("probe_asm_ret") "cmp x30, x16\nb.eq 1f\n" PROBE_LOAD("probe_asm_next") "br x16\n" \
  "1:\nstp x29, x30, [sp, #-16]!\n" PROBE_CFI(".cfi_def_cfa_offset 16") PROBE_CFI(".cfi_offset 29, -16")       \
  PROBE_CFI(".cfi_offset 30, -8") "mov x29, sp\nbl probe_asm_continuation\nldp x29, x30, [sp], #16\n"           \
  PROBE_CFI(".cfi_restore 29") PROBE_CFI(".cfi_restore 30") PROBE_CFI(".cfi_def_cfa_offset 0") "ret\n"

#if defined(PROBE_NAKED)
extern "C" __attribute__((naked, visibility("hidden"))) int probe_asm_entry(int) { asm volatile(PROBE_ENTRY_BODY); }
#else
asm(".text\n.p2align 2\n.hidden probe_asm_entry\n.type probe_asm_entry,%function\nprobe_asm_entry:\n"
    PROBE_CFI(".cfi_startproc") PROBE_ENTRY_BODY PROBE_CFI(".cfi_endproc") ".size probe_asm_entry, .-probe_asm_entry\n");
#endif

namespace {
NEVR_ASM_ENTRY_RECORD(kProbeEntry, probe_asm_entry, probe_asm_continuation);
}  // namespace

int main() {
  sentinel::GotHook hook;
  const sentinel::GotTarget target{"libprobe.so", "probe", sentinel::RelocKind::kJumpSlot};
  return sentinel::InstallAsmEntry(hook, target, kProbeEntry, &probe_asm_next) == sentinel::GotStatus::kOk ? 0 : 1;
}

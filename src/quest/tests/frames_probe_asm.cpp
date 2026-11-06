// Built -fno-exceptions. A reviewed assembly entry (sentinel/callback_thunk.h rule 7) whose continuation calls a helper with a
// personality directly, across the call into the "game": the frame sensor's controls for the entry kind
// (tests/quest frames_sensor_test.go). Never run: the sensor reads the linked executable.
//
//   frames_probe_asm_bad     the helper is NOT annotated: it is live across the call, so the sensor MUST fail it
//   frames_probe_asm_ok      the helper is annotated NEVR_OUTSIDE_GAME_CALL and the entry carries CFI: the sensor MUST pass it
//   frames_probe_asm_nocfi   the entry has no CFI, so no FDE and a game exception cannot unwind through it: the sensor MUST fail it
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

#if defined(PROBE_NO_CFI)
#define PROBE_CFI(directive) ""
#else
#define PROBE_CFI(directive) directive "\n"
#endif

asm(R"(
  .text
  .p2align 2
  .hidden probe_asm_entry
  .type probe_asm_entry,%function
probe_asm_entry:
)" PROBE_CFI(".cfi_startproc") R"(
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
)" PROBE_CFI(".cfi_def_cfa_offset 16") PROBE_CFI(".cfi_offset 29, -16") PROBE_CFI(".cfi_offset 30, -8") R"(
  mov x29, sp
  bl probe_asm_continuation
  ldp x29, x30, [sp], #16
)" PROBE_CFI(".cfi_restore 29") PROBE_CFI(".cfi_restore 30") PROBE_CFI(".cfi_def_cfa_offset 0") R"(
  ret
)" PROBE_CFI(".cfi_endproc") R"(
  .size probe_asm_entry, .-probe_asm_entry
)");

namespace {
NEVR_ASM_ENTRY_RECORD(kProbeEntry, probe_asm_entry, probe_asm_continuation);
}  // namespace

int main() {
  sentinel::GotHook hook;
  const sentinel::GotTarget target{"libprobe.so", "probe", sentinel::RelocKind::kJumpSlot};
  return sentinel::InstallAsmEntry(hook, target, kProbeEntry, &probe_asm_next) == sentinel::GotStatus::kOk ? 0 : 1;
}

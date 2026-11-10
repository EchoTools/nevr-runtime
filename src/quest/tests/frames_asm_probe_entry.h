// Built -fno-exceptions. The part every single-entry assembly probe shares (tests/frames_asm_probes/*.cpp): the entry's
// record, its continuation, the two words the entry reads, and a main that installs it. A probe file adds only the entry's text,
// a top-level asm() block that defines probe_asm_entry. Never run: the frame sensor (tests/quest frames_sensor_test.go)
// reads the linked executable.
#pragma once

#include "hook_install.h"

namespace probe {
// Defined in frames_probe_helper_ok.cpp (exceptions enabled), annotated NEVR_OUTSIDE_GAME_CALL.
int LiveHelper(int value) noexcept;
}  // namespace probe

extern "C" {
int probe_asm_entry(int value);
void* probe_asm_ret = nullptr;   // the game's call site (the return address the entry acts on)
void* probe_asm_next = nullptr;  // what the entry jumps to for every other call; the manager publishes it
int probe_asm_continuation(int value) noexcept { return probe::LiveHelper(value); }
}

namespace {
NEVR_ASM_ENTRY_RECORD(kProbeEntry, probe_asm_entry, probe_asm_continuation);
}  // namespace

int main() {
  sentinel::GotHook hook;
  const sentinel::GotTarget target{"libprobe.so", "probe", sentinel::RelocKind::kJumpSlot};
  return sentinel::InstallAsmEntry(hook, target, kProbeEntry, &probe_asm_next) == sentinel::GotStatus::kOk ? 0 : 1;
}

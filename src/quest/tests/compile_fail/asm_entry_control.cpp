// Control: the supported way to declare and install a reviewed assembly entry compiles. asm_entry_direct_record.cpp and
// asm_entry_raw_install.cpp differ from it by one thing each and must NOT compile.
#include "common.h"

NEVR_ASM_ENTRY_RECORD(kEntry, snippet::Entry, snippet::Continuation);

void* g_next = nullptr;

sentinel::GotStatus Use(sentinel::GotHook& hook, const sentinel::GotTarget& target) {
  return sentinel::InstallAsmEntry(hook, target, kEntry, &g_next);
}

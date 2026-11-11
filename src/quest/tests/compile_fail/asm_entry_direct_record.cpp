// Must not compile: an AsmEntryRecord cannot be built outside NEVR_ASM_ENTRY_RECORD, so an entry the frame sensor does not
// walk cannot be installed.
#include "common.h"

void* g_next = nullptr;

sentinel::GotStatus Use(sentinel::GotHook& hook, const sentinel::GotTarget& target) {
  const sentinel::AsmEntryRecord<decltype(&snippet::Entry), decltype(&snippet::Continuation)> record{&snippet::Entry,
                                                                                                    &snippet::Continuation};
  return sentinel::InstallAsmEntry(hook, target, record, &g_next);
}

// Must not compile: the entry install of GotHook is private. Only AsmEntryInstaller reaches it, and it takes a record.
#include "common.h"

void* g_next = nullptr;

sentinel::GotStatus Use(sentinel::GotHook& hook, const sentinel::GotTarget& target) {
  return hook.InstallEntry(target, reinterpret_cast<void*>(&snippet::Entry), &g_next, sentinel::FindLoadedImage);
}

// Must not compile: InstallThunk takes only a CallbackThunk instantiation, not any type that
// happens to have EntryAddress() and OriginalOut().
#include "common.h"

struct FakeThunk {
  static void* EntryAddress() { return nullptr; }
  static void** OriginalOut() { return nullptr; }
};

sentinel::GotStatus Use(sentinel::GotHook& hook, const sentinel::GotTarget& target) {
  return sentinel::InstallThunk<FakeThunk>(hook, target);
}

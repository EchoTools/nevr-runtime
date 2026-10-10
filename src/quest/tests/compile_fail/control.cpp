// Control: the supported way to define, arm and install a hook compiles. The other snippets in
// this directory differ from it by one thing each and must NOT compile.
#include "common.h"

NEVR_HOOK_RECORD(kHook, snippet::Thunk, &snippet::Handler);

sentinel::GotStatus Use(sentinel::GotHook& hook, const sentinel::GotTarget& target) {
  snippet::Thunk::Arm(kHook);
  return sentinel::InstallThunk<snippet::Thunk>(hook, target);
}

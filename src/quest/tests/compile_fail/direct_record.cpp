// Must not compile: a HookRecord cannot be built outside NEVR_HOOK_RECORD.
#include "common.h"

void Use() {
  sentinel::HookRecord<snippet::Thunk> record{snippet::Thunk::EntryFn(), &snippet::Handler};
  snippet::Thunk::Arm(record);
}

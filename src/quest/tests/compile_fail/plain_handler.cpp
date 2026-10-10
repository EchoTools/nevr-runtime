// Must not compile: a handler that is not declared noexcept does not convert to a Handler.
#include "common.h"

int NotNoexcept(snippet::Thunk::Fn original, int a, int b) { return original(a, b); }

NEVR_HOOK_RECORD(kHook, snippet::Thunk, &NotNoexcept);

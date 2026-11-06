// Shared by the compile-fail snippets: a legitimate thunk and handler.
#pragma once
#include <time.h>

#include "hook_install.h"

namespace snippet {
struct Tag {};
using Thunk = sentinel::CallbackThunk<Tag, int(int, int)>;
inline int Handler(Thunk::Fn original, int a, int b) noexcept { return original(a, b); }
}  // namespace snippet

namespace snippet {
// A reviewed assembly entry's stand-ins (the control and the compile-fail snippets of NEVR_ASM_ENTRY_RECORD).
inline int Continuation(int result) noexcept { return result; }
inline int Entry(int a, int b) { return Continuation(a + b); }
}  // namespace snippet

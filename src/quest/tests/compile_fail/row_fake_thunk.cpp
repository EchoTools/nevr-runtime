// Must not compile: InstallRowThunk takes only a CallbackThunk instantiation, not any type that happens to have
// EntryAddress() and OriginalOut(), so a row's detour is always a thunk entry the frame sensor walks.
#include "common.h"

struct FakeThunk {
  static void* EntryAddress() { return nullptr; }
  static void** OriginalOut() { return nullptr; }
};

nevr_quest::libr15_patch::ActivateStatus Use() { return sentinel::InstallRowThunk<FakeThunk>("lobby_full.join"); }

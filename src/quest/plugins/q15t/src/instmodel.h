// instmodel (docs/quest/q15t/modules/instmodel.md): CGInstancedModelResource's constructor, with the growth step of its eight
// CTableXT members set to 1 instead of the stock 32. The stock constructor runs unchanged; the wrapper sets the eight steps after it
// returns, and only where the stock value is still there. The constructor is reached through one GOT word (a JUMP_SLOT read by a PLT
// stub), so the hook is a data-page swap, not a code write.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

// Installs the wrapper on the constructor's GOT word. Refused (nothing changed) on the off file (instmodel.off), on a word pin that does
// not read back, or when the slot does not hold the constructor (InstallImportHook's own refusal).
InstallResult InstallInstModel(std::uintptr_t base);

// Test seams: the stock constructor the wrapper calls, and the wrapper itself.
using InstModelCtorFn = void (*)(void* self, const void* init);
void SetInstModelRealForTest(InstModelCtorFn real);
void InstModelCtorWrapper(void* self, const void* init);
void ResetInstModelForTest();

}  // namespace q15t

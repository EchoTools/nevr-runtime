// Stub out dbghelp.dll functions pulled in by abseil's symbolize library.
// The real dbghelp.dll depends on dbgcore.dll, which conflicts with our
// gamepatches DLL deployed as dbgcore.dll via DLL hijacking.
//
// Abseil's compiled code uses __declspec(dllimport), so it references
// __imp_Sym* directly. We provide both the function and the __imp_
// pointer so the linker resolves locally instead of importing dbghelp.dll.

#include <windows.h>

extern "C" {

static BOOL WINAPI StubSymInitialize(HANDLE, PCSTR, BOOL) { return FALSE; }
static BOOL WINAPI StubSymFromAddr(HANDLE, DWORD64, DWORD64 *, void *) {
  return FALSE;
}
static DWORD WINAPI StubSymSetOptions(DWORD) { return 0; }
static BOOL WINAPI StubSymCleanup(HANDLE) { return FALSE; }

void *__imp_SymInitialize = (void *)StubSymInitialize;
void *__imp_SymFromAddr = (void *)StubSymFromAddr;
void *__imp_SymSetOptions = (void *)StubSymSetOptions;
void *__imp_SymCleanup = (void *)StubSymCleanup;

} // extern "C"

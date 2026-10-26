// The nine MiniDmpSender methods echovr.exe imports from BugSplat64.dll. The game imports them by their
// MSVC-mangled names; src/runtime/exports.def maps each mangled name to the C function here. They are in
// their own translation unit so the crash harness (src/runtime/tests/host_crash) links the same bodies the
// shipped DLL exports and not a copy of them.
#include "runtime/host/host_entry.h"

#include <windows.h>

extern "C" {
void BugSplat_MiniDmpSender_Ctor(void*, const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*,
                                 unsigned long) {
  nevr_host::OnCrashReporterConstructed();  // reports the attach decisions recorded so far
}
void BugSplat_MiniDmpSender_Dtor(void*) {}
void BugSplat_createReport(void*, PEXCEPTION_POINTERS) {}
unsigned long BugSplat_getFlags(void*) { return 0; }
void BugSplat_resetAppIdentifier(void*, const wchar_t*) {}
void BugSplat_sendAdditionalFile(void*, const wchar_t*) {}
void BugSplat_setDefaultUserDescription(void*, const wchar_t*) {}
void BugSplat_setDefaultUserName(void*, const wchar_t*) {}
int BugSplat_setFlags(void*, unsigned long) { return 1; }
}

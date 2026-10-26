// nevr-runtime.dll fixture with no NEVR_GetRuntimeApi export: exercises the host's
// "missing entry point" path.

#include <windows.h>

extern "C" __declspec(dllexport) DWORD WINAPI NotTheRuntimeEntry() {
  return ERROR_PROC_NOT_FOUND;
}

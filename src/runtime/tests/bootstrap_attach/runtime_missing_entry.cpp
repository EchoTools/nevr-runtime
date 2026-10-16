#include <windows.h>

extern "C" __declspec(dllexport) DWORD WINAPI NotTheRuntimeEntry() {
  return ERROR_PROC_NOT_FOUND;
}

// A stand-in platform DLL for test_export_trace: built as pnsrad.dll (in its own directory), with one code
// export and one data export, so the tracer is tested against a real module's real exports.
#include <windows.h>

extern "C" {
__declspec(dllexport) int FixtureCode(int x) { return x + 1; }
__declspec(dllexport) int FixtureData = 1234;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

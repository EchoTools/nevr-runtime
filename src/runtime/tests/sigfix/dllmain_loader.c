/* A plugin that loads another library from its own DllMain: the file named by NEVR_TEST_DLLMAIN_LOAD. It exports no
 * plugin entry points; what is under test is the load it makes while the loader still holds the loader lock. */
#include <stddef.h>

__declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableW(const wchar_t *, wchar_t *, unsigned long);
__declspec(dllimport) void *__stdcall LoadLibraryW(const wchar_t *);

int __stdcall DllMain(void *module, unsigned long reason, void *reserved)
{
    if (reason == 1) {
        wchar_t path[520];
        unsigned long n = GetEnvironmentVariableW(L"NEVR_TEST_DLLMAIN_LOAD", path, 520);
        if (n > 0 && n < 520) {
            LoadLibraryW(path);
        }
    }
    return 1;
}

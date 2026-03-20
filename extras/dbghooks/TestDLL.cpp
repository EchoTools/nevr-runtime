#include <windows.h>
#include <stdio.h>

// Minimal test DLL - just writes a file on load
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        FILE* f = fopen("injection_test_success.txt", "w");
        if (f) {
            fprintf(f, "Test DLL loaded successfully!\n");
            fprintf(f, "Module handle: %p\n", hModule);
            fclose(f);
        }
    }
    return TRUE;
}

/* SYNTHESIS -- custom tool code, not from binary
 *
 * DLL Load Interceptor
 *
 * Hooks LoadLibraryA/W to intercept DLL loads at runtime. Components
 * register interest in specific DLL names via OnLoad(). When a matching
 * DLL loads, the registered callback fires with the HMODULE — allowing
 * post-load patching before the game uses the DLL.
 *
 * Pattern:
 *   DllLoadHook::Install();  // early in Initialize()
 *   DllLoadHook::OnLoad("pnsdemo.dll", PatchPnsDemoUserId);
 *   // ... later, game loads pnsdemo.dll ...
 *   // PatchPnsDemoUserId(name, hmod) fires automatically
 */

#include "dll_load_hook.h"
#include "core/hooking.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <MinHook.h>
#endif

namespace DllLoadHook {

struct Registration {
    char dll_name[64];     // lowercase, filename only
    PatchCallback callback;
    bool fired;
};

static std::vector<Registration> g_registrations;

#ifdef _WIN32

/* Original function pointers */
static decltype(&LoadLibraryA) g_origLoadLibraryA = nullptr;
static decltype(&LoadLibraryW) g_origLoadLibraryW = nullptr;
static decltype(&LoadLibraryExA) g_origLoadLibraryExA = nullptr;
static decltype(&LoadLibraryExW) g_origLoadLibraryExW = nullptr;

/* Extract filename from a path and lowercase it */
static void ExtractLowerFilename(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) { out[0] = '\0'; return; }

    const char* last_sep = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') last_sep = p + 1;
    }
    if (last_sep == path && *path != '\\' && *path != '/') last_sep = path;

    size_t i = 0;
    for (; last_sep[i] && i < out_size - 1; ++i) {
        char c = last_sep[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
    }
    out[i] = '\0';
}

static void ExtractLowerFilenameW(const wchar_t* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) { out[0] = '\0'; return; }

    const wchar_t* last_sep = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') last_sep = p + 1;
    }
    if (last_sep == path && *path != L'\\' && *path != L'/') last_sep = path;

    size_t i = 0;
    for (; last_sep[i] && i < out_size - 1; ++i) {
        wchar_t c = last_sep[i];
        out[i] = (c >= L'A' && c <= L'Z') ? (char)(c + 32) : (char)c;
    }
    out[i] = '\0';
}

/* Fire registered callbacks for a loaded DLL (each fires at most once) */
static void FireCallbacks(const char* lower_name, HMODULE module) {
    for (auto& reg : g_registrations) {
        if (!reg.fired && strcmp(reg.dll_name, lower_name) == 0) {
            fprintf(stderr, "[NEVR.DLLHOOK] firing patch callback for '%s' (module=%p)\n",
                    lower_name, (void*)module);
            fflush(stderr);
            reg.fired = true;
            reg.callback(lower_name, module);
        }
    }
}

/* Hook implementations */
static HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpFileName) {
    HMODULE result = g_origLoadLibraryA(lpFileName);
    if (result && lpFileName) {
        char lower[64];
        ExtractLowerFilename(lpFileName, lower, sizeof(lower));
        FireCallbacks(lower, result);
    }
    return result;
}

static HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpFileName) {
    HMODULE result = g_origLoadLibraryW(lpFileName);
    if (result && lpFileName) {
        char lower[64];
        ExtractLowerFilenameW(lpFileName, lower, sizeof(lower));
        FireCallbacks(lower, result);
    }
    return result;
}

static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE result = g_origLoadLibraryExA(lpFileName, hFile, dwFlags);
    if (result && lpFileName && !(dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        char lower[64];
        ExtractLowerFilename(lpFileName, lower, sizeof(lower));
        FireCallbacks(lower, result);
    }
    return result;
}

static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE result = g_origLoadLibraryExW(lpFileName, hFile, dwFlags);
    if (result && lpFileName && !(dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        char lower[64];
        ExtractLowerFilenameW(lpFileName, lower, sizeof(lower));
        FireCallbacks(lower, result);
    }
    return result;
}

#endif // _WIN32

void Install() {
#ifdef _WIN32
    auto hook = [](void* target, void* detour, void** original) {
        return MH_CreateHook(target, detour, original) == MH_OK &&
               MH_EnableHook(target) == MH_OK;
    };

    bool ok = true;
    ok &= hook((void*)&LoadLibraryA,   (void*)&HookedLoadLibraryA,   (void**)&g_origLoadLibraryA);
    ok &= hook((void*)&LoadLibraryW,   (void*)&HookedLoadLibraryW,   (void**)&g_origLoadLibraryW);
    ok &= hook((void*)&LoadLibraryExA, (void*)&HookedLoadLibraryExA, (void**)&g_origLoadLibraryExA);
    ok &= hook((void*)&LoadLibraryExW, (void*)&HookedLoadLibraryExW, (void**)&g_origLoadLibraryExW);

    fprintf(stderr, "[NEVR.DLLHOOK] LoadLibrary hooks %s\n", ok ? "OK" : "PARTIAL");
    fflush(stderr);
#endif
}

void Shutdown() {
#ifdef _WIN32
    if (g_origLoadLibraryA)   MH_DisableHook((void*)&LoadLibraryA);
    if (g_origLoadLibraryW)   MH_DisableHook((void*)&LoadLibraryW);
    if (g_origLoadLibraryExA) MH_DisableHook((void*)&LoadLibraryExA);
    if (g_origLoadLibraryExW) MH_DisableHook((void*)&LoadLibraryExW);
#endif
    g_registrations.clear();
}

void OnLoad(const char* dll_name, PatchCallback callback) {
    Registration reg;
    ExtractLowerFilename(dll_name, reg.dll_name, sizeof(reg.dll_name));
    reg.callback = callback;
    reg.fired = false;
    g_registrations.push_back(reg);

    // If the DLL is already loaded, fire immediately
    HMODULE existing = GetModuleHandleA(dll_name);
    if (existing) {
        fprintf(stderr, "[NEVR.DLLHOOK] '%s' already loaded (module=%p), firing immediately\n",
                reg.dll_name, (void*)existing);
        fflush(stderr);
        g_registrations.back().fired = true;
        callback(reg.dll_name, existing);
    }
}

void FireCallbacksForModule(const char* lower_name, HMODULE module) {
    FireCallbacks(lower_name, module);
}

} // namespace DllLoadHook

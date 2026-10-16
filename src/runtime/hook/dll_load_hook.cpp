/* SYNTHESIS -- custom tool code, not from binary
 *
 * DLL Load Interceptor
 *
 * Hooks LoadLibraryA/W/ExA/ExW to intercept DLL loads at runtime. Components
 * register interest in specific DLL names via OnLoad(). When a matching
 * DLL loads, the registered callback fires with the HMODULE — allowing
 * post-load patching before the game uses the DLL.
 *
 * Pattern:
 *   nevr_dll_load_hook::Install();  // early in Initialize()
 *   nevr_dll_load_hook::OnLoad("pnsdemo.dll", PatchPnsDemoUserId);
 *   // ... later, game loads pnsdemo.dll ...
 *   // PatchPnsDemoUserId(name, hmod) fires automatically
 */

#include "runtime/hook/dll_load_hook.h"
#include "runtime/hook/hook_guard.h"
#include "core/hooking.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#include <MinHook.h>
#include <winternl.h>  // TEB::ProcessEnvironmentBlock
#endif

namespace nevr_dll_load_hook {

struct Registration {
    char dll_name[64];     // lowercase, filename only
    PatchCallback callback;
    bool fired;
    std::uint64_t id;
};

static std::vector<Registration> g_registrations;
static std::mutex g_registryMutex;
static std::uint64_t g_nextRegistrationId = 0;

struct Filter {
    char name[48];
    LoadFilter predicate;
};
static std::vector<Filter> g_filters;

static std::vector<PostLoadObserver> g_observers;

#ifdef _WIN32

/* Original function pointers */
static decltype(&LoadLibraryA) g_origLoadLibraryA = nullptr;
static decltype(&LoadLibraryW) g_origLoadLibraryW = nullptr;
static decltype(&LoadLibraryExA) g_origLoadLibraryExA = nullptr;
static decltype(&LoadLibraryExW) g_origLoadLibraryExW = nullptr;

#ifdef NEVR_DLL_LOAD_HOOK_TESTING
static ModuleProbe g_moduleProbe = &GetModuleHandleA;

void SetModuleProbeForTest(ModuleProbe probe) {
    g_moduleProbe = probe ? probe : &GetModuleHandleA;
}

static HMODULE FindLoadedModule(const char* name) { return g_moduleProbe(name); }
#else
static HMODULE FindLoadedModule(const char* name) { return GetModuleHandleA(name); }
#endif

#ifdef NEVR_DLL_LOAD_HOOK_TESTING
static MinHookOps g_hookOps = {&MH_CreateHook, &MH_EnableHook, &MH_RemoveHook};

void SetMinHookOpsForTest(const MinHookOps* ops) {
    g_hookOps = {&MH_CreateHook, &MH_EnableHook, &MH_RemoveHook};
    if (ops == nullptr) return;
    if (ops->create) g_hookOps.create = ops->create;
    if (ops->enable) g_hookOps.enable = ops->enable;
    if (ops->remove) g_hookOps.remove = ops->remove;
}

void* OriginalForTest(const char* name) {
    if (strcmp(name, "LoadLibraryA") == 0) return reinterpret_cast<void*>(g_origLoadLibraryA);
    if (strcmp(name, "LoadLibraryW") == 0) return reinterpret_cast<void*>(g_origLoadLibraryW);
    if (strcmp(name, "LoadLibraryExA") == 0) return reinterpret_cast<void*>(g_origLoadLibraryExA);
    if (strcmp(name, "LoadLibraryExW") == 0) return reinterpret_cast<void*>(g_origLoadLibraryExW);
    return nullptr;
}

static MH_STATUS HookCreate(void* target, void* detour, void** original) {
    return g_hookOps.create(target, detour, original);
}
static MH_STATUS HookEnable(void* target) { return g_hookOps.enable(target); }
static MH_STATUS HookRemove(void* target) { return g_hookOps.remove(target); }
#else
static MH_STATUS HookCreate(void* target, void* detour, void** original) {
    return MH_CreateHook(target, detour, original);
}
static MH_STATUS HookEnable(void* target) { return MH_EnableHook(target); }
static MH_STATUS HookRemove(void* target) { return MH_RemoveHook(target); }
#endif

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

/* Lowercased wide copy of an ANSI path (ASCII fold, like the other extractors here). */
static std::wstring LowerWide(const char* path) {
    std::wstring out;
    for (const char* p = path; p && *p; ++p) {
        char c = *p;
        out.push_back(static_cast<wchar_t>((c >= 'A' && c <= 'Z') ? c + 32 : c));
    }
    return out;
}

static std::wstring LowerWide(const wchar_t* path) {
    std::wstring out;
    for (const wchar_t* p = path; p && *p; ++p) {
        wchar_t c = *p;
        out.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c);
    }
    return out;
}

/* Returns true (and sets ERROR_MOD_NOT_FOUND) when a filter refuses this load. */
static bool RefuseLoad(const std::wstring& lower) {
    const char* by = nullptr;
    if (!IsLoadBlocked(lower.c_str(), &by)) return false;
    const std::string filterName = by ? by : "?";
    fprintf(stderr, "[NEVR.DLLHOOK] load refused by filter '%s': %ls\n", filterName.c_str(), lower.c_str());
    fflush(stderr);
    SetLastError(ERROR_MOD_NOT_FOUND);
    return true;
}

/* Fire registered callbacks for a loaded DLL (each fires at most once) */
static void FireCallbacks(const char* lower_name, HMODULE module) {
    std::vector<Registration> pending;
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        pending.reserve(g_registrations.size());
        for (auto& reg : g_registrations) {
            if (!reg.fired && strcmp(reg.dll_name, lower_name) == 0) {
                reg.fired = true;
                pending.push_back(reg);
            }
        }
    }
    for (const auto& reg : pending) {
        fprintf(stderr, "[NEVR.DLLHOOK] firing patch callback for '%s' (module=%p)\n",
                reg.dll_name, (void*)module);
        fflush(stderr);
        reg.callback(reg.dll_name, module);
    }
}

/* The PEB's LoaderLock field (an RTL_CRITICAL_SECTION*) on x64. winternl.h's PEB names it Reserved. */
static constexpr size_t kPebLoaderLockOffset = 0x110;

/* Run every post-load observer, unless this thread holds the loader lock. Copies the list first, so an
 * observer may load a library or register another observer. */
static void RunPostLoadObservers() {
    if (LoaderLockHeldByThisThread()) return;
    std::vector<PostLoadObserver> observers;
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        observers = g_observers;
    }
    for (const auto observer : observers) observer();
}

/* Hook implementations */
static HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpFileName) {
    if (lpFileName && RefuseLoad(LowerWide(lpFileName))) return nullptr;
    HMODULE result = g_origLoadLibraryA(lpFileName);
    RunPostLoadObservers();
    if (result && lpFileName) {
        char lower[64];
        ExtractLowerFilename(lpFileName, lower, sizeof(lower));
        FireCallbacks(lower, result);
    }
    return result;
}

static HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpFileName) {
    if (lpFileName && RefuseLoad(LowerWide(lpFileName))) return nullptr;
    HMODULE result = g_origLoadLibraryW(lpFileName);
    RunPostLoadObservers();
    if (result && lpFileName) {
        char lower[64];
        ExtractLowerFilenameW(lpFileName, lower, sizeof(lower));
        FireCallbacks(lower, result);
    }
    return result;
}

static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpFileName && RefuseLoad(LowerWide(lpFileName))) return nullptr;
    HMODULE result = g_origLoadLibraryExA(lpFileName, hFile, dwFlags);
    RunPostLoadObservers();
    if (result && lpFileName && !(dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        char lower[64];
        ExtractLowerFilename(lpFileName, lower, sizeof(lower));
        FireCallbacks(lower, result);
    }
    return result;
}

static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpFileName && RefuseLoad(LowerWide(lpFileName))) return nullptr;
    HMODULE result = g_origLoadLibraryExW(lpFileName, hFile, dwFlags);
    RunPostLoadObservers();
    if (result && lpFileName && !(dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        char lower[64];
        ExtractLowerFilenameW(lpFileName, lower, sizeof(lower));
        FireCallbacks(lower, result);
    }
    return result;
}

#endif // _WIN32

void AddLoadFilter(const char* name, LoadFilter filter) {
    if (!filter) return;
    Filter f{};
    const char* label = name ? name : "?";
    const int needed = snprintf(f.name, sizeof(f.name), "%s", label);
    if (needed >= static_cast<int>(sizeof(f.name))) {
        fprintf(stderr, "[NEVR.DLLHOOK] load filter name truncated to %zu bytes: '%s'\n",
                sizeof(f.name) - 1, f.name);
        fflush(stderr);
    }
    f.predicate = filter;
    std::lock_guard<std::mutex> lock(g_registryMutex);
    g_filters.push_back(f);
}

bool IsOculusPlatformPath(const wchar_t* lower_path) {
    if (!lower_path) return false;
    const wchar_t* leaf = lower_path;
    for (const wchar_t* p = lower_path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
    }
    return std::wcsstr(leaf, L"ovrplatform") != nullptr;
}

bool IsLoadBlocked(const wchar_t* lower_path, const char** blocked_by) {
    if (!lower_path) return false;
    std::vector<Filter> filters;
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        filters = g_filters;
    }
    for (const auto& f : filters) {
        if (f.predicate(lower_path)) {
            if (blocked_by) {
                // A fixed per-thread buffer: a thread_local std::string would register a
                // per-thread destructor in this DLL. f.name is already NUL-terminated and
                // truncated to sizeof(Filter::name) - 1 bytes by AddLoadFilter.
                static thread_local char name[sizeof(Filter::name)];
                std::memcpy(name, f.name, sizeof(name));
                name[sizeof(name) - 1] = '\0';
                *blocked_by = name;
            }
            return true;
        }
    }
    return false;
}

bool LoaderLockHeldByThisThread() {
#ifdef _WIN32
    const auto* peb = reinterpret_cast<const std::uint8_t*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    const auto* loaderLock = *reinterpret_cast<RTL_CRITICAL_SECTION* const*>(peb + kPebLoaderLockOffset);
    if (loaderLock == nullptr) return false;
    // OwningThread holds the owner's thread id (not a handle) while the section is entered, 0 otherwise.
    return reinterpret_cast<std::uintptr_t>(loaderLock->OwningThread) ==
           static_cast<std::uintptr_t>(GetCurrentThreadId());
#else
    return false;
#endif
}

void AddPostLoadObserver(PostLoadObserver observer) {
    if (!observer) return;
    std::lock_guard<std::mutex> lock(g_registryMutex);
    g_observers.push_back(observer);
}

void Install() {
#ifdef _WIN32
    // N129. These four hooks run the load filters (AddLoadFilter), post-load
    // observers (AddPostLoadObserver) and DLL-load callbacks (OnLoad) the runtime
    // registers, each under its own condition (the Hooked* functions above). They
    // restrict no search path (N75's is the plugin loader's own load flags). A single
    // "OK"/"PARTIAL" across all four variants would hide WHICH variant failed and WHY:
    // loads through an unhooked variant run no filter, observer or callback. Report
    // each variant and its MH_STATUS, mirroring N126/N128 for the PatchDetour path.
    // (This path does not go through PatchDetour, so it needs its own reporting.)
    struct HookSpec { const char* name; void* target; void* detour; void** original; };
    const HookSpec specs[] = {
        {"LoadLibraryA",   (void*)&LoadLibraryA,   (void*)&HookedLoadLibraryA,   (void**)&g_origLoadLibraryA},
        {"LoadLibraryW",   (void*)&LoadLibraryW,   (void*)&HookedLoadLibraryW,   (void**)&g_origLoadLibraryW},
        {"LoadLibraryExA", (void*)&LoadLibraryExA, (void*)&HookedLoadLibraryExA, (void**)&g_origLoadLibraryExA},
        {"LoadLibraryExW", (void*)&LoadLibraryExW, (void*)&HookedLoadLibraryExW, (void**)&g_origLoadLibraryExW},
    };

    int okCount = 0;
    for (const auto& s : specs) {
        const MH_STATUS created = HookCreate(s.target, s.detour, s.original);
        const MH_STATUS st = created == MH_OK ? HookEnable(s.target) : created;
        if (st == MH_OK) {
            nevr_hook_guard::Record(s.target, s.name);
            okCount++;
        } else {
            // MinHook writes the trampoline only when MH_CreateHook returns MH_OK. When
            // creation itself failed (for example MH_ERROR_ALREADY_CREATED on a second
            // Install()), *s.original still holds the working trampoline of the hook that
            // is installed, and the detour calls through it: nulling it would crash the
            // next LoadLibrary. Only a hook this call created and then removed has its
            // pointer cleared.
            if (created == MH_OK) {
                const MH_STATUS cleanup = HookRemove(s.target);
                if (cleanup != MH_OK) {
                    fprintf(stderr, "[NEVR.DLLHOOK] failed-hook cleanup FAILED name=%s reason=%s\n",
                            s.name, MH_StatusToString(cleanup));
                }
                *s.original = nullptr;
            }
            // A create that failed because the hook already exists leaves that hook live.
            const char* effect = created == MH_ERROR_ALREADY_CREATED
                                     ? "a hook created earlier stays in place"
                                     : "loads through this variant run no load filter, observer or callback";
            fprintf(stderr, "[NEVR.DLLHOOK] hook FAILED name=%s reason=%s — %s\n", s.name,
                    MH_StatusToString(st), effect);
        }
    }

    // Keep "LoadLibrary hooks OK" as a substring on full success (smoke row B09),
    // and always report the count so PARTIAL is never a bare word without a number.
    fprintf(stderr, "[NEVR.DLLHOOK] LoadLibrary hooks %s (%d/4)\n",
            okCount == 4 ? "OK" : "PARTIAL", okCount);
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
    std::lock_guard<std::mutex> lock(g_registryMutex);
    g_registrations.clear();
    g_filters.clear();
    g_observers.clear();
}

void OnLoad(const char* dll_name, PatchCallback callback) {
    Registration reg{};
    ExtractLowerFilename(dll_name, reg.dll_name, sizeof(reg.dll_name));
    reg.callback = callback;
    // If the DLL is already loaded, fire immediately
    HMODULE existing = FindLoadedModule(dll_name);
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        reg.id = ++g_nextRegistrationId;
        g_registrations.push_back(reg);
    }

    if (existing) {
        bool claimed = false;
        {
            std::lock_guard<std::mutex> lock(g_registryMutex);
            for (auto& registered : g_registrations) {
                if (registered.id == reg.id && !registered.fired) {
                    registered.fired = true;
                    claimed = true;
                    break;
                }
            }
        }
        if (claimed) {
            fprintf(stderr, "[NEVR.DLLHOOK] '%s' already loaded (module=%p), firing immediately\n",
                    reg.dll_name, (void*)existing);
            fflush(stderr);
            callback(reg.dll_name, existing);
        }
    }
}

void FireCallbacksForModule(const char* lower_name, HMODULE module) {
    FireCallbacks(lower_name, module);
}

} // namespace nevr_dll_load_hook

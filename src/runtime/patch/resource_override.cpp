/*
 * resource_override.cpp — Hook AsyncResourceIOCallback to replace game resources.
 *
 * Provides a registry-based system for overriding game resources by their
 * type and name symbol hashes. Overrides can be either embedded data (compiled
 * into the DLL) or loaded from disk at runtime.
 *
 * Hook target: AsyncResourceIOCallback @ 0x140fa16d0
 * The callback receives SIORequestCallbackData:
 *   +0x00: status (4 = success)
 *   +0x08: CResource* pointer
 *   +0x10: buffer index (0 = primary, 1 = secondary)
 *   +0x18: data buffer pointer
 *   +0x20: data size
 *
 * CResource struct layout:
 *   +0x28: type_symbol  (uint64 hash)
 *   +0x38: name_symbol  (uint64 hash)
 */

#include "runtime/patch/resource_override.h"
#include "runtime/hook/symbol_corpus.h"
#include "runtime/hook/hook_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <windows.h>
#include <MinHook.h>

#include "core/globals.h"
#include "core/logging.h"

/* Embedded assets */
#include "runtime/patch/splash_texture.h"

namespace {

/* ── Override registry ────────────────────────────────────────────── */

struct ResourceOverride {
    uint64_t type_hash;     /* 0 = match any type */
    uint64_t name_hash;
    const void* data;       /* embedded data (not owned) or VirtualAlloc'd (owned) */
    uint64_t size;
    bool owned;             /* true if data was allocated and should be freed */
    const char* label;      /* for logging */
    bool applied;           /* set to true after first replacement */
    uint32_t in_flight;     /* game callbacks still using this replacement */
};

static std::unique_ptr<std::vector<std::shared_ptr<ResourceOverride>>> g_overrides;
static uint64_t g_total_hits = 0;
// The game callback can call back into this API, so never hold this mutex
// while calling it. Deregistration waits for selected entries to leave it.
static std::mutex g_overrides_mutex;
static std::condition_variable g_overrides_idle;
static std::mutex g_hook_install_mutex;
constexpr DWORD kReentrantDeregisterExitCode = 0xE0000478;
struct ActiveOverrideFrame {
    ResourceOverride* selected;
    ActiveOverrideFrame* previous;
};
static thread_local ActiveOverrideFrame* g_active_override = nullptr;
static void (*g_fatal_line_sink)(const char*) = nullptr;

/* ── Known asset hashes ───────────────────────────────────────────── */

constexpr uint64_t TYPE_DDS_TEXTURE     = 0xbeac1969cb7b8861ULL;
constexpr uint64_t NAME_SPLASH_TEXTURE  = 0x1f72c5e0fc9fc8c0ULL;

/* ── Hook ─────────────────────────────────────────────────────────── */

typedef void (__cdecl* AsyncIOCallback_fn)(void* callback_data);
static AsyncIOCallback_fn g_orig = nullptr;

struct CallbackLease {
    std::shared_ptr<ResourceOverride> selected;
    ActiveOverrideFrame frame;

    explicit CallbackLease(std::shared_ptr<ResourceOverride> entry)
        : selected(std::move(entry)), frame{selected.get(), g_active_override} {
        g_active_override = &frame;
    }

    ~CallbackLease() {
        g_active_override = frame.previous;
        if (selected) {
            std::lock_guard<std::mutex> lock(g_overrides_mutex);
            --selected->in_flight;
            g_overrides_idle.notify_all();
        }
    }
};

static void __cdecl Hook_AsyncIOCallback(void* callback_data) {
    std::shared_ptr<ResourceOverride> selected;
    AsyncIOCallback_fn original;
    uint64_t type_hash = 0, name_hash = 0, orig_size = 0;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        original = g_orig;
        if (callback_data && g_overrides && !g_overrides->empty()) {
            auto cbd = reinterpret_cast<uintptr_t>(callback_data);
            void* resource = *reinterpret_cast<void**>(cbd + 0x08);
            uint32_t buf_index = *reinterpret_cast<uint32_t*>(cbd + 0x10);

            if (resource && buf_index == 0) {
                auto res = reinterpret_cast<uintptr_t>(resource);
                name_hash = *reinterpret_cast<uint64_t*>(res + 0x38);
                type_hash = *reinterpret_cast<uint64_t*>(res + 0x28);

                for (auto& ovr : *g_overrides) {
                    if (ovr->applied) continue;
                    if (ovr->name_hash != name_hash) continue;
                    if (ovr->type_hash != 0 && ovr->type_hash != type_hash) continue;

                    /* Match — replace the buffer */
                    orig_size = *reinterpret_cast<uint64_t*>(cbd + 0x20);
                    *reinterpret_cast<const void**>(cbd + 0x18) = ovr->data;
                    *reinterpret_cast<uint64_t*>(cbd + 0x20) = ovr->size;
                    ovr->applied = true;
                    ++ovr->in_flight;
                    selected = ovr;
                    g_total_hits++;
                    break;
                }
            }
        }
    }
    CallbackLease lease(selected);
    if (selected) {
        // Resolve both hashes to human names the same way registration does.
        char typeBuf[128], nameBuf[128];
        const char* typeName = EchoVR::LookupSymbolName(type_hash);
        const char* resName = EchoVR::LookupSymbolName(name_hash);
        snprintf(typeBuf, sizeof(typeBuf), "0x%016llx%s%s",
                 static_cast<unsigned long long>(type_hash),
                 typeName ? " (" : "", typeName ? typeName : "");
        if (typeName) strncat(typeBuf, ")", sizeof(typeBuf) - strlen(typeBuf) - 1);
        snprintf(nameBuf, sizeof(nameBuf), "0x%016llx%s%s",
                 static_cast<unsigned long long>(name_hash),
                 resName ? " (" : "", resName ? resName : "");
        if (resName) strncat(nameBuf, ")", sizeof(nameBuf) - strlen(nameBuf) - 1);
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] override applied label=%s type=%s name=%s orig_bytes=%llu "
            "new_bytes=%llu",
            selected->label, typeBuf, nameBuf,
            static_cast<unsigned long long>(orig_size),
            static_cast<unsigned long long>(selected->size));
    }
    original(callback_data);
}

/* ── File loading helper ──────────────────────────────────────────── */

static void* LoadFileFromDisk(const char* path, uint64_t* out_size, DWORD* outError = nullptr) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (outError) *outError = GetLastError();
        return nullptr;
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) {
        if (outError) *outError = GetLastError();
        CloseHandle(hFile);
        return nullptr;
    }

    uint64_t sz = static_cast<uint64_t>(li.QuadPart);
    void* buf = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        if (outError) *outError = GetLastError();
        CloseHandle(hFile);
        return nullptr;
    }

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buf, static_cast<DWORD>(sz), &bytesRead, NULL);
    if (outError) *outError = GetLastError();
    CloseHandle(hFile);

    if (!ok || bytesRead != sz) { VirtualFree(buf, 0, MEM_RELEASE); return nullptr; }

    *out_size = sz;
    return buf;
}

/* ── Lazy hook installation ───────────────────────────────────────── */

static void EnsureHookInstalled() {
    std::lock_guard<std::mutex> installLock(g_hook_install_mutex);
    // Memoizes SUCCESS (g_orig) only — without this latch, a persistent
    // failure (bad hook target) re-attempts and re-logs an identical Warning
    // on every RegisterResourceOverride call for the rest of the process
    // (once for the built-in splash texture, once per _overrides/ file,
    // again at the end of InstallResourceOverride, and again for every
    // plugin that later calls the exported NEVR_RegisterResourceOverride).
    static bool s_hookAttemptFailed = false;
    {
        std::lock_guard<std::mutex> registryLock(g_overrides_mutex);
        if (g_orig) return;  /* Already installed */
    }
    if (s_hookAttemptFailed) return;  /* Already tried and failed this session */

    // This DLL is loaded for the process lifetime by the game. MinHook cannot
    // drain a callback that has entered our detour, and DllMain cannot wait for
    // worker threads under the loader lock. Pin before exposing the detour so
    // FreeLibrary cannot unmap it while a callback is active.
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCSTR>(&Hook_AsyncIOCallback), &self)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] runtime pin failed error=%lu — resource hook disabled",
            GetLastError());
        s_hookAttemptFailed = true;
        return;
    }
    Log(EchoVR::LogLevel::Info, "[NEVR.RESOURCE] runtime pinned for resource hook");

    void* hookTarget = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) + 0xFA16D0);
    AsyncIOCallback_fn original = nullptr;

    MH_STATUS cst = MH_CreateHook(hookTarget, reinterpret_cast<void*>(Hook_AsyncIOCallback),
                                  reinterpret_cast<void**>(&original));
    if (cst != MH_OK) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] hook create failed target=%p status=%s — resource overrides will not "
            "be applied",
            hookTarget, MH_StatusToString(cst));
        s_hookAttemptFailed = true;
        return;
    }

    // Publish the trampoline before enabling the target; a callback can begin
    // as soon as MinHook restores the other threads.
    {
        std::lock_guard<std::mutex> registryLock(g_overrides_mutex);
        g_orig = original;
    }
    MH_STATUS est = MH_EnableHook(hookTarget);
    if (est != MH_OK) {
        MH_STATUS rst = MH_RemoveHook(hookTarget);
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] hook enable failed target=%p status=%s cleanup=%s — resource overrides "
            "will not be applied",
            hookTarget, MH_StatusToString(est), MH_StatusToString(rst));
        {
            std::lock_guard<std::mutex> registryLock(g_overrides_mutex);
            g_orig = nullptr;
        }
        s_hookAttemptFailed = true;
        return;
    }

    nevr_hook_guard::Record(hookTarget, "AsyncResourceIOCallback");
    Log(EchoVR::LogLevel::Debug, "[NEVR.RESOURCE] hook installed target=%p", hookTarget);
}

} // anonymous namespace

/* ── Public API ───────────────────────────────────────────────────── */

void RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                              const void* data, uint64_t size,
                              const char* label) {
    auto ovr = std::make_shared<ResourceOverride>();
    ovr->type_hash = type_hash;
    ovr->name_hash = name_hash;
    ovr->data = data;
    ovr->size = size;
    ovr->owned = false;
    ovr->label = label;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return;
        g_overrides->push_back(ovr);
    }

    /* Lazily install hook if this is the first override after init */
    EnsureHookInstalled();

    char typeBuf[128], nameBuf[128];
    const char* typeName = EchoVR::LookupSymbolName(type_hash);
    const char* resName = EchoVR::LookupSymbolName(name_hash);
    snprintf(typeBuf, sizeof(typeBuf), "0x%016llx%s%s",
             static_cast<unsigned long long>(type_hash),
             typeName ? " (" : "", typeName ? typeName : "");
    if (typeName) strncat(typeBuf, ")", sizeof(typeBuf) - strlen(typeBuf) - 1);
    snprintf(nameBuf, sizeof(nameBuf), "0x%016llx%s%s",
             static_cast<unsigned long long>(name_hash),
             resName ? " (" : "", resName ? resName : "");
    if (resName) strncat(nameBuf, ")", sizeof(nameBuf) - strlen(nameBuf) - 1);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] Registered embedded override: %s (type=%s name=%s, %llu bytes)",
        label,
        typeBuf,
        nameBuf,
        static_cast<unsigned long long>(size));
}

static bool RegisterResourceOverrideFromFileImpl(uint64_t type_hash, uint64_t name_hash,
                                                  const char* file_path,
                                                  const char* label) {
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return false;
    }
    uint64_t size = 0;
    DWORD err = 0;
    void* data = LoadFileFromDisk(file_path, &size, &err);
    if (!data) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] override file load failed path=%s error=%lu", file_path, err);
        return false;
    }
    auto ovr = std::make_shared<ResourceOverride>();
    ovr->type_hash = type_hash;
    ovr->name_hash = name_hash;
    ovr->data = data;
    ovr->size = size;
    ovr->owned = true;
    ovr->label = label;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) {
            VirtualFree(data, 0, MEM_RELEASE);
            return false;
        }
        g_overrides->push_back(ovr);
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] Registered file override: %s (%s, %llu bytes)",
        label, file_path, static_cast<unsigned long long>(size));
    return true;
}

void RegisterResourceOverrideFromFile(uint64_t type_hash, uint64_t name_hash,
                                      const char* file_path,
                                      const char* label) {
    RegisterResourceOverrideFromFileImpl(type_hash, name_hash, file_path, label);
}

void InstallResourceOverride() {
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (g_overrides) return;
        g_overrides = std::make_unique<std::vector<std::shared_ptr<ResourceOverride>>>();
    }
    size_t overridesFailed = 0;

    /* ── Register built-in overrides ──────────────────────────────── */

    /* Splash screen texture (EchoVRCE branding) */
    RegisterResourceOverride(
        TYPE_DDS_TEXTURE, NAME_SPLASH_TEXTURE,
        kSplashDDS, kSplashDDS_len,
        "splash_texture");

    /* ── Scan _overrides/ directory for additional files ──────────── */

    char moduleDir[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, moduleDir, MAX_PATH);
    char* lastSlash = strrchr(moduleDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char overrideDir[MAX_PATH];
    snprintf(overrideDir, sizeof(overrideDir), "%s_overrides", moduleDir);

    WIN32_FIND_DATAA fd;
    char searchPattern[MAX_PATH];
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*", overrideDir);
    HANDLE hFind = FindFirstFileA(searchPattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const char* name = fd.cFileName;

            /* Parse "0xTYPE.0xNAME" or "0xNAME" filename format */
            uint64_t type_h = 0, name_h = 0;
            if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
                const char* dot = strchr(name, '.');
                if (dot && dot[1] == '0' && (dot[2] == 'x' || dot[2] == 'X')) {
                    type_h = strtoull(name + 2, nullptr, 16);
                    name_h = strtoull(dot + 3, nullptr, 16);
                } else {
                    name_h = strtoull(name + 2, nullptr, 16);
                }
            }

            if (name_h == 0) continue;

            char filePath[MAX_PATH];
            snprintf(filePath, sizeof(filePath), "%s\\%s", overrideDir, name);
            if (!RegisterResourceOverrideFromFileImpl(type_h, name_h, filePath, name)) {
                overridesFailed++;
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    /* Install hook if overrides exist; otherwise keep g_overrides alive
     * for plugins that register overrides later via the exported API. */
    size_t registered;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        registered = g_overrides->size();
    }
    if (registered != 0) {
        EnsureHookInstalled();
    }

    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] init complete overrides_registered=%zu overrides_failed=%zu",
        registered, overridesFailed);
}

void ResetResourceOverrides() {
    uint64_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return;
        for (auto& ovr : *g_overrides) {
            if (ovr->applied) {
                ovr->applied = false;
                count++;
            }
        }
    }
    if (count > 0) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] Reset %llu override(s) for re-application",
            static_cast<unsigned long long>(count));
    }
}

void SetResourceOverrideFatalLineSink(void (*sink)(const char* line)) {
    g_fatal_line_sink = sink;
}

ResourceOverrideShutdownReport ShutdownResourceOverride() {
    ResourceOverrideShutdownReport report = {};
    std::unique_ptr<std::vector<std::shared_ptr<ResourceOverride>>> released;
    {
        std::lock_guard<std::mutex> lock(g_overrides_mutex);
        if (!g_overrides) return report;
        report.registered = g_overrides->size();
        report.hook_installed = g_orig != nullptr;
        for (const auto& ovr : *g_overrides) {
            if (ovr->applied) continue;
            ++report.never_applied;
            // A registered-but-never-matched override (wrong hash, or the expected resource genuinely never
            // loaded this session) must not read the same as one that worked.
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.RESOURCE] override never applied label=%s name=0x%016llx — resource was never requested "
                "this session",
                ovr->label, static_cast<unsigned long long>(ovr->name_hash));
        }
        // With the hook live the module is pinned and a callback may be running: keep the registry. With no
        // hook nothing can reach the buffers, so release them.
        if (!report.hook_installed) released = std::move(g_overrides);
    }
    if (released) {
        for (const auto& ovr : *released) {
            if (ovr->owned && ovr->data) VirtualFree(const_cast<void*>(ovr->data), 0, MEM_RELEASE);
        }
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] shutdown complete overrides_registered=%llu overrides_never_applied=%llu hook_installed=%s "
        "registry_released=%s",
        static_cast<unsigned long long>(report.registered), static_cast<unsigned long long>(report.never_applied),
        report.hook_installed ? "true" : "false", report.hook_installed ? "false" : "true");
    return report;
}

void DeregisterResourceOverrides(const void* data_start, const void* data_end) {
    std::unique_lock<std::mutex> lock(g_overrides_mutex);
    if (!g_overrides) return;
    auto start = reinterpret_cast<uintptr_t>(data_start);
    auto end = reinterpret_cast<uintptr_t>(data_end);
    std::vector<std::shared_ptr<ResourceOverride>> retired;

    auto it = g_overrides->begin();
    while (it != g_overrides->end()) {
        auto ptr = reinterpret_cast<uintptr_t>((*it)->data);
        if (ptr >= start && ptr < end) {
            retired.push_back(*it);
            it = g_overrides->erase(it);
        } else {
            ++it;
        }
    }

    for (auto* frame = g_active_override; frame; frame = frame->previous) {
        for (const auto& ovr : retired) {
            if (frame->selected != ovr.get()) continue;
            // The void plugin API cannot safely return while the caller may
            // unload bytes that this callback still uses. Log() allocates and
            // takes locks, and this can run with arbitrary game/plugin locks
            // held, so the one record goes out through a raw, non-allocating
            // sink (the crash record's WriteFile path) before the process ends.
            char line[192];
            snprintf(line, sizeof(line),
                     "[NEVR.RESOURCE] FATAL reentrant deregistration: label=%s was deregistered from inside its own "
                     "game callback; terminating with exit code 0x%lX",
                     ovr->label ? ovr->label : "(null)", static_cast<unsigned long>(kReentrantDeregisterExitCode));
            if (g_fatal_line_sink) {
                g_fatal_line_sink(line);
            } else {
                fputs(line, stderr);
                fputc('\n', stderr);
                fflush(stderr);
            }
            TerminateProcess(GetCurrentProcess(), kReentrantDeregisterExitCode);
            std::abort();
        }
    }

    // Future callbacks cannot select these entries. The game callback may
    // still be reading their bytes, so wait without holding the registry lock.
    g_overrides_idle.wait(lock, [&] {
        for (const auto& ovr : retired) {
            if (ovr->in_flight != 0) return false;
        }
        return true;
    });
    lock.unlock();
    for (const auto& ovr : retired) {
        if (ovr->owned && ovr->data) {
            VirtualFree(const_cast<void*>(ovr->data), 0, MEM_RELEASE);
        }
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] Deregistered override: %s", ovr->label);
    }
    if (!retired.empty()) {
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RESOURCE] Deregistered %llu override(s)",
            static_cast<unsigned long long>(retired.size()));
    }
}

/* ── Exported wrappers (called by plugins via GetProcAddress) ────── */

extern "C" {

void NEVR_RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                                   const void* data, uint64_t size,
                                   const char* label) {
    RegisterResourceOverride(type_hash, name_hash, data, size, label);
}

void NEVR_ResetResourceOverrides() {
    ResetResourceOverrides();
}

void NEVR_DeregisterResourceOverrides(const void* data_start,
                                      const void* data_end) {
    DeregisterResourceOverrides(data_start, data_end);
}

} /* extern "C" */

#ifdef NEVR_TEST_HOOKS
void TestHook_InitResourceOverrides(void (*original)(void*)) {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    g_overrides = std::make_unique<std::vector<std::shared_ptr<ResourceOverride>>>();
    g_orig = original;
}

void TestHook_SetOriginal(void (*original)(void*)) {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    g_orig = original;
}

void TestHook_RunResourceCallback(void* callback_data) {
    Hook_AsyncIOCallback(callback_data);
}

void TestHook_CloseResourceOverrides() {
    std::lock_guard<std::mutex> lock(g_overrides_mutex);
    g_overrides.reset();
    g_orig = nullptr;
}
#endif

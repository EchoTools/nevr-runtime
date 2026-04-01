#include "module_loader.h"
#include "Globals.h"
#include "logging.h"
#include "config.h"
#include "../Core/CBaseErr.h"
#include "../Core/TLSMemory.h"

#include <cstring>

/* @module: pnsrad.dll */

// Forward declarations for internal helpers used by init functions
extern "C" void memset_wrapper(void* ptr, int val, uint64_t size);  // @0x1800897f0
extern void* get_tls_context();      // @0x180089700
extern void  release_lock(void* lock); // @0x180089a00

// Environment method accessors (return stored globals)
extern void* get_env_method_1();     // @0x180089ac0
extern void* get_env_method_2();     // @0x180089860
extern void* get_debug_method_1();   // @0x180089850
extern void* get_debug_method_2();   // @0x180089830
extern void* get_debug_method_3();   // @0x180089840
extern void* get_debug_method_4();   // @0x180089820
extern void* get_system_info_extra(); // @0x180093da0
extern char* get_system_info_1();    // @0x180093920
extern char* get_system_info_2();    // @0x1800939c0

// Error system
extern int64_t get_error_buffer();             // @0x18008a740
extern uint64_t report_error(NRadEngine::CBasicErr* err); // @0x18008b4a0
extern void error_handler(int32_t* err_code);  // @0x180096a20

// Internal string helpers
extern int64_t str_compare(const char* a, const char* b, int32_t case_sensitive, int64_t max_len); // @0x180092c30
extern bool str_ends_with(const char* str, const char* suffix, int32_t case_sensitive); // @0x180092cd0
extern int64_t str_find_char(const char* str, int32_t ch, int32_t case_sensitive, int64_t start); // @0x180093080
extern int64_t strnlen_wrapper(const char* str, int64_t max_len); // @0x180093120
extern void wide_to_narrow(char* out, const wchar_t* in, int64_t max_len); // @0x180092e10
extern void memcpy_wrapper(void* dst, const void* src, uint64_t size); // @0x1800899f0

// Global: module root path (0x200 bytes)
/* @addr: 0x180380d10 (pnsrad.dll) */
extern char g_module_root_path[0x200];

// Global: module initialized flag
/* @addr: 0x1803808e0 (pnsrad.dll) */
extern uint32_t g_module_initialized;

// Global: empty string sentinel used for comparisons
/* @addr: 0x180222db4 (pnsrad.dll) */
extern const char g_empty_string[];

// Registry path for environment default
/* @addr: 0x180228e60 (pnsrad.dll) */
extern const char g_default_registry_path[];

// Sentinel file names tested during config walk
/* @addr: 0x18022a42c (pnsrad.dll) */ extern const char g_sentinel_radconfig[];
/* @addr: 0x18022a434 (pnsrad.dll) */ extern const char g_sentinel_dev[];
/* @addr: 0x18022a43c (pnsrad.dll) */ extern const char g_sentinel_release[];
/* @addr: 0x18022a444 (pnsrad.dll) */ extern const char g_sentinel_staging[];
/* @addr: 0x18022a44c (pnsrad.dll) */ extern const char g_sentinel_production[];

// Path/dir resolution
extern int64_t path_dir_exists(const char* path);   // @0x1800943d0
extern int64_t config_file_walk_up(int64_t out_buf, int64_t in_buf);   // @0x1800a2050
extern int64_t config_file_walk_down(int64_t out_buf, int64_t in_buf); // @0x1800a2170

// Error log format for missing config
/* @addr: 0x18022a450 (pnsrad.dll) */
extern const char g_missing_config_fmt[];

namespace NRadEngine {

// @0x180091ea0 — InvokeHostSymbol (inlined pattern from InitNonMemoryStatics)
// Extracts the repeated lock/resolve/invoke/error pattern.  The
// decompilation shows identical structure for each RadPluginSet* call:
//   ScopedModuleLockAcquire -> check null -> CBasicErr / call -> error_handler -> release
/* @addr: 0x180091ea0 (pnsrad.dll) */ /* @confidence: H */
static void InvokeHostSymbol(FARPROC proc, void* arg) {
    uint8_t lock[16];
    ScopedModuleLockAcquire(lock);

    int32_t result;
    if (proc == nullptr) {
        CBasicErr err;
        result = static_cast<int32_t>(report_error(&err));
    } else {
        result = static_cast<int32_t>(
            reinterpret_cast<int64_t(*)(void*)>(proc)(arg));
    }

    if (result != 0) {
        error_handler(&result);
    }
    ScopedModuleLockRelease();
}

// @0x1800953d0 — GetProcAddress wrapper
/* @addr: 0x1800953d0 (pnsrad.dll) */ /* @confidence: H */
FARPROC ResolveSymbol(HMODULE hModule, const char* symbol_name) {
    char error_buf[0x80];

    if (symbol_name != nullptr && hModule != nullptr) {
        FARPROC proc = GetProcAddress(hModule, symbol_name);
        if (proc != nullptr) {
            return proc;
        }
        memset_wrapper(error_buf, 0, 0x80);
        extern void get_last_error(char* buf); // @0x180093f40
        get_last_error(error_buf);
    }
    return nullptr;
}

// @0x180095420 — Get the directory containing the given module
/* @addr: 0x180095420 (pnsrad.dll) */ /* @confidence: H */
void GetModuleDirectory(HMODULE hModule, char* out_dir) {
    wchar_t wide_path[0x200];
    wchar_t drive[0x200];
    wchar_t dir[0x200];

    GetModuleFileNameW(hModule, wide_path, 0x200);
    _wsplitpath_s(wide_path, nullptr, 0, dir, 0x200, nullptr, 0, nullptr, 0);

    wide_to_narrow(out_dir, dir, 0x200);

    int64_t pos = str_find_char(out_dir, '\\', 1, -1);
    if (pos != -1) {
        out_dir[pos] = '\0';
    }
}

// @0x180095510 — Module symbol setup / config search loop
// Copies the module directory into a working buffer, then walks upward
// through parent directories looking for sentinel config files.
// When a match is found, the resolved path is stored into g_module_root_path.
/* @addr: 0x180095510 (pnsrad.dll) */ /* @confidence: H */
void SetupModuleSymbols(const char* module_dir) {
    char work_buf[0x200];
    char parent_buf[0x200];
    char search_buf[0x200];

    memcpy(work_buf, module_dir, 0x200);
    memset_wrapper(search_buf, 0, 0x200);

    // Walk upward through parent directories looking for sentinel files
    int64_t found;
    do {
        char* resolved = reinterpret_cast<char*>(config_file_walk_up(
            reinterpret_cast<int64_t>(parent_buf),
            reinterpret_cast<int64_t>(work_buf)));
        memcpy(search_buf, resolved, 0x200);

        resolved = reinterpret_cast<char*>(config_file_walk_down(
            reinterpret_cast<int64_t>(parent_buf),
            reinterpret_cast<int64_t>(work_buf)));
        memcpy(work_buf, resolved, 0x200);

        found = str_compare(search_buf, g_empty_string, 1, 0x200);
    } while (found != 0
             && !str_ends_with(search_buf, g_sentinel_radconfig, 1)
             && !str_ends_with(search_buf, g_sentinel_dev, 1)
             && !str_ends_with(search_buf, g_sentinel_release, 1)
             && !str_ends_with(search_buf, g_sentinel_staging, 1)
             && !str_ends_with(search_buf, g_sentinel_production, 1));

    // Second pass: verify the resolved path
    found = str_compare(search_buf, g_empty_string, 1, 0x200);
    if (found == 0) {
        // No sentinel found -- fall back to registry/environment default
        log_message(8, 0, g_missing_config_fmt, module_dir);

        uint64_t len = strnlen_wrapper(g_default_registry_path, 0);
        if (len < 0x201) {
            memcpy_wrapper(g_module_root_path, g_default_registry_path, len);
            if (len < 0x200) {
                g_module_root_path[len] = '\0';
            }
        } else {
            memcpy_wrapper(g_module_root_path, g_default_registry_path, 0x200);
        }
        g_module_root_path[0x1FF] = '\0';

        if (len + 1 < 0x200) {
            memset(&g_module_root_path[len + 1], 0, 0x200 - (len + 1));
        }
    } else {
        // Sentinel found -- copy the resolved parent path
        char* resolved = reinterpret_cast<char*>(config_file_walk_down(
            reinterpret_cast<int64_t>(parent_buf),
            reinterpret_cast<int64_t>(work_buf)));
        memcpy(g_module_root_path, resolved, 0x200);
    }
}

// @0x180095c20 — Scoped module lock acquire
/* @addr: 0x180095c20 (pnsrad.dll) */ /* @confidence: H */
void* ScopedModuleLockAcquire(void* result_out) {
    int64_t env = get_error_buffer();
    if (env != 0) {
        *reinterpret_cast<uint32_t*>(result_out) = 0;
    }
    return result_out;
}

// @0x180095cb0 — Scoped module lock release
/* @addr: 0x180095cb0 (pnsrad.dll) */ /* @confidence: H */
void ScopedModuleLockRelease() {
    get_error_buffer();
}

// @0x180091df0 — Init memory statics
// Resolves RadPluginSetAllocator from the host module and invokes it
// with the global allocator pointer.
/* @addr: 0x180091df0 (pnsrad.dll) */ /* @confidence: H */
void InitMemoryStatics(HMODULE hModule) {
    FARPROC pfnSetAllocator = ResolveSymbol(hModule, "RadPluginSetAllocator");
    InvokeHostSymbol(pfnSetAllocator, g_allocator_ptr);
}

// @0x180091ea0 — Init non-memory statics
// Resolves all RadPluginSet* functions from the host module and invokes
// each with the corresponding global state.
/* @addr: 0x180091ea0 (pnsrad.dll) */ /* @confidence: H */
void InitNonMemoryStatics(HMODULE hModule) {
    InvokeHostSymbol(ResolveSymbol(hModule, "RadPluginSetPresenceFactory"), g_presence_factory_ptr);
    InvokeHostSymbol(ResolveSymbol(hModule, "RadPluginSetFileTypes"), g_file_types_ptr);
    InvokeHostSymbol(ResolveSymbol(hModule, "RadPluginSetEnvironment"), g_environment_ptr);

    // SetEnvironmentMethods takes two args
    {
        FARPROC proc = ResolveSymbol(hModule, "RadPluginSetEnvironmentMethods");
        uint8_t lock[16];
        ScopedModuleLockAcquire(lock);
        void* m1 = get_env_method_1();
        void* m2 = get_env_method_2();
        int32_t result;
        if (proc == nullptr) {
            CBasicErr err;
            result = static_cast<int32_t>(report_error(&err));
        } else {
            result = static_cast<int32_t>(
                reinterpret_cast<int64_t(*)(void*, void*)>(proc)(m2, m1));
        }
        if (result != 0) { error_handler(&result); }
        ScopedModuleLockRelease();
    }

    // SetSymbolDebugMethodsMethod takes four args
    {
        FARPROC proc = ResolveSymbol(hModule, "RadPluginSetSymbolDebugMethodsMethod");
        uint8_t lock[16];
        ScopedModuleLockAcquire(lock);
        void* d1 = get_debug_method_1();
        void* d2 = get_debug_method_2();
        void* d3 = get_debug_method_3();
        void* d4 = get_debug_method_4();
        int32_t result;
        if (proc == nullptr) {
            CBasicErr err;
            result = static_cast<int32_t>(report_error(&err));
        } else {
            result = static_cast<int32_t>(
                reinterpret_cast<int64_t(*)(void*, void*, void*, void*)>(proc)(d4, d3, d2, d1));
        }
        if (result != 0) { error_handler(&result); }
        ScopedModuleLockRelease();
    }

    // SetSystemInfo takes three args
    {
        FARPROC proc = ResolveSymbol(hModule, "RadPluginSetSystemInfo");
        uint8_t lock[16];
        ScopedModuleLockAcquire(lock);
        void*  extra = get_system_info_extra();
        char*  info2 = get_system_info_2();
        char*  info1 = get_system_info_1();
        int32_t result;
        if (proc == nullptr) {
            CBasicErr err;
            result = static_cast<int32_t>(report_error(&err));
        } else {
            result = static_cast<int32_t>(
                reinterpret_cast<int64_t(*)(char*, char*, void*)>(proc)(info1, info2, extra));
        }
        if (result != 0) { error_handler(&result); }
        ScopedModuleLockRelease();
    }
}

// @0x180092310 — Module initialization (plugin loader)
/* @addr: 0x180092310 (pnsrad.dll) */ /* @confidence: H */
void ModuleInit(HMODULE hModule, uint64_t param_1, uint64_t param_2, uint64_t param_3) {
    config_init_tls(nullptr);

    void* tls_ctx = get_tls_context();
    if (tls_ctx == nullptr) {
        if (g_allocator_ptr == nullptr) {
            // log_message at level 8 (kLogError) does not return
            log_message(8, 0,
                "Trying to use memory context before it's initialized!",
                param_3);
        }
        release_lock(*reinterpret_cast<void**>(g_allocator_ptr));
    }

    if (g_init_counter < 2) {
        char module_dir[0x200];
        memset_wrapper(module_dir, 0, 0x200);
        GetModuleDirectory(hModule, module_dir);
        SetupModuleSymbols(module_dir);
        g_module_initialized = 1;
    }
}

} // namespace NRadEngine

/* SYNTHESIS -- custom tool code, not from binary */

#include "filesystem_loader.h"
#include "nevr_plugin_interface.h"
#include "nevr_common.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

static constexpr const char* PLUGIN_NAME = "filesystem_loader";
static constexpr const char* PLUGIN_DESC =
    "Intercepts resource loading to serve modified files from disk "
    "instead of compressed archive packages.";

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[NEVR.FSLOAD] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

static std::string FindConfigFile() {
    const char* candidates[] = {
        "filesystem_loader_config.json",
    };

#ifdef _WIN32
    char dll_path[MAX_PATH] = {};
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&FindConfigFile), &hm)) {
        GetModuleFileNameA(hm, dll_path, MAX_PATH);
        char* last_slash = std::strrchr(dll_path, '\\');
        if (!last_slash) last_slash = std::strrchr(dll_path, '/');
        if (last_slash) {
            *(last_slash + 1) = '\0';
            for (auto* name : candidates) {
                std::string path = std::string(dll_path) + name;
                FILE* f = std::fopen(path.c_str(), "r");
                if (f) { std::fclose(f); return path; }
            }
        }
    }
#endif
    for (auto* name : candidates) {
        FILE* f = std::fopen(name, "r");
        if (f) { std::fclose(f); return name; }
    }
    return {};
}

/*
 * Resolve override_dir to an absolute path.
 * If it's relative, resolve it relative to the game binary's directory.
 */
static std::string ResolveOverrideDir(const std::string& configured_dir) {
#ifdef _WIN32
    /* Check if already absolute (drive letter or UNC) */
    if (configured_dir.size() >= 2 &&
        ((configured_dir[1] == ':') ||
         (configured_dir[0] == '\\' && configured_dir[1] == '\\'))) {
        return configured_dir;
    }

    /* Resolve relative to game binary directory */
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    char* last_slash = std::strrchr(exe_path, '\\');
    if (!last_slash) last_slash = std::strrchr(exe_path, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0';
    }
    return std::string(exe_path) + configured_dir;
#else
    if (!configured_dir.empty() && configured_dir[0] == '/') {
        return configured_dir;
    }
    /* On non-Windows, just use relative path as-is */
    return configured_dir;
#endif
}

/* ── Plugin interface exports ─────────────────────────────────────── */

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    return NvrPluginInfo{
        PLUGIN_NAME,
        PLUGIN_DESC,
        /* version */ 1, 0, 0
    };
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    if (!ctx) return -1;

    /* Load config */
    nevr::filesystem_loader::LoaderConfig cfg;
    std::string config_path = FindConfigFile();
    if (!config_path.empty()) {
        std::string content = nevr::LoadConfigFile(config_path);
        if (!content.empty()) {
            cfg = nevr::filesystem_loader::ParseConfig(content);
            Log("config loaded from %s: override_dir=%s",
                config_path.c_str(), cfg.override_dir.c_str());
        }
    }

    if (!cfg.valid) {
        Log("no config file found, using default override_dir: %s",
            cfg.override_dir.c_str());
    }

    /* Resolve override directory */
    std::string override_dir = ResolveOverrideDir(cfg.override_dir);
    Log("scanning override directory: %s", override_dir.c_str());

    /* Install hook (also allocates runtime state) */
    if (!nevr::filesystem_loader::InstallHook(ctx->base_addr)) {
        Log("failed to install hook");
        return -1;
    }

    /* Scan for override files */
    int count = nevr::filesystem_loader::ScanOverrideDirectory(override_dir);
    if (count == 0) {
        Log("no override files found — plugin will be a no-op");
        /* Hook stays installed so overrides can be hot-added later
         * by placing files in the directory and restarting */
    }
    Log("loaded %d override file(s)", count);

    Log("initialization complete");
    return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    Log("shutting down");
    nevr::filesystem_loader::RemoveHook();
}

/* ── DllMain for standalone injection ─────────────────────────────── */

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)hModule;
    (void)reserved;

    /*
     * Do NOT call Initialize/Shutdown here.
     * The plugin loader calls LoadLibraryA (which triggers DllMain) and then
     * calls NvrPluginInit separately. Auto-init from DllMain caused
     * double-initialization, crashes, and wrong timing.
     */
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif

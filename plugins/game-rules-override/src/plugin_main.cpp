/* SYNTHESIS -- custom tool code, not from binary */

#include "game_rules_override.h"
#include "nevr_plugin_interface.h"
#include "nevr_common.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <MinHook.h>
#endif

/* --------------------------------------------------------------------
 * Logging
 * -------------------------------------------------------------------- */

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[game_rules_override] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

/* --------------------------------------------------------------------
 * Config file discovery
 * -------------------------------------------------------------------- */

static std::string FindConfigFile() {
    const char* filename = "game_rules_override.jsonc";

#ifdef _WIN32
    /* Try next to the DLL first */
    char dll_path[MAX_PATH] = {};
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&FindConfigFile), &hm)) {
        GetModuleFileNameA(hm, dll_path, MAX_PATH);
        /* Strip the DLL filename to get directory */
        char* last_slash = std::strrchr(dll_path, '\\');
        if (!last_slash) last_slash = std::strrchr(dll_path, '/');
        if (last_slash) {
            *(last_slash + 1) = '\0';
            std::string path = std::string(dll_path) + filename;
            FILE* f = std::fopen(path.c_str(), "r");
            if (f) {
                std::fclose(f);
                return path;
            }
        }
    }
#endif

    /* Fall back to current working directory */
    FILE* f = std::fopen(filename, "r");
    if (f) {
        std::fclose(f);
        return filename;
    }

    return {};
}

/* --------------------------------------------------------------------
 * Plugin state
 * -------------------------------------------------------------------- */

static GameRulesOverrideConfig s_config;
static uintptr_t s_base_addr = 0;
static bool s_hook_installed = false;
static bool s_overrides_applied = false;

/* --------------------------------------------------------------------
 * CJsonGetFloat hook — intercepts arena timing reads from game JSON
 * -------------------------------------------------------------------- */

#ifdef _WIN32
using CJsonGetFloat_t = float(__cdecl*)(void* root, const char* path, float defaultValue, int32_t required);
static CJsonGetFloat_t s_originalCJsonGetFloat = nullptr;

static int s_hook_call_count = 0;

static float __cdecl CJsonGetFloatHook(void* root, const char* path, float defaultValue, int32_t required) {
    float result = s_originalCJsonGetFloat(root, path, defaultValue, required);
    s_hook_call_count++;

    if (path != nullptr) {
        /* Log every call that contains any arena-related keyword */
        if (std::strstr(path, "round") || std::strstr(path, "celebration") ||
            std::strstr(path, "mercy") || std::strstr(path, "time") ||
            std::strstr(path, "score") || std::strstr(path, "arena") ||
            std::strstr(path, "combat") || std::strstr(path, "rule")) {
            Log("CJsonGetFloat[%d]: path=\"%s\" default=%.2f result=%.2f", s_hook_call_count, path, defaultValue, result);
        }

        if (s_config.point_score_celebration_time >= 0.0f &&
            std::strstr(path, "point_score_celebration_time") != nullptr &&
            std::strstr(path, "_private") == nullptr) {
            Log("OVERRIDE point_score_celebration_time: %.2f -> %.2f", result, s_config.point_score_celebration_time);
            return s_config.point_score_celebration_time;
        }
        if (s_config.round_time >= 0.0f &&
            std::strstr(path, "round_time") != nullptr &&
            std::strstr(path, "_private") == nullptr &&
            std::strstr(path, "round_timer") == nullptr &&
            std::strstr(path, "sudden_death_round_time") == nullptr) {
            Log("OVERRIDE round_time: %.2f -> %.2f", result, s_config.round_time);
            return s_config.round_time;
        }
        if (s_config.mercy_win_point_spread >= 0.0f &&
            std::strstr(path, "mercy_win_point_spread") != nullptr &&
            std::strstr(path, "_private") == nullptr) {
            Log("OVERRIDE mercy_win_point_spread: %.2f -> %.2f", result, s_config.mercy_win_point_spread);
            return s_config.mercy_win_point_spread;
        }
    }

    return result;
}
#endif

/* --------------------------------------------------------------------
 * Plugin interface exports
 * -------------------------------------------------------------------- */

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "game_rules_override";
    info.description = "Patches EchoVR balance config struct at runtime from a JSON config file";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    Log("initializing (base=0x%llx)", static_cast<unsigned long long>(ctx->base_addr));

    s_base_addr = ctx->base_addr;

    /* Find and load config */
    std::string config_path = FindConfigFile();
    if (config_path.empty()) {
        Log("no config file found (game_rules_override.jsonc)");
        return -1;
    }

    s_config = LoadConfig(config_path.c_str());
    if (!s_config.valid) {
        Log("config parse failed");
        return -2;
    }

    /* Verify addresses */
    if (!VerifyAddresses(ctx->base_addr)) {
        Log("address verification failed -- wrong binary version?");
        return -3;
    }

    /* Try to apply balance struct overrides now; if the struct isn't
       allocated yet, we'll retry on the first game state change. */
    if (ApplyOverrides(ctx->base_addr, s_config)) {
        s_overrides_applied = true;
    } else {
        Log("balance config not ready yet, deferring to game state change");
    }

#ifdef _WIN32
    /* Install CJsonGetFloat hook for arena timing overrides */
    /* CJsonGetFloat hook disabled — causes clients to fail to connect.
       Arena timing overrides need a different approach (replicated vars or
       direct memory patch). */
    bool has_timing = false && (s_config.round_time >= 0.0f ||
                      s_config.point_score_celebration_time >= 0.0f ||
                      s_config.mercy_win_point_spread >= 0.0f);
    if (has_timing) {
        void* target = nevr::ResolveVA(ctx->base_addr, nevr::addresses::VA_CJSON_GET_FLOAT);
        MH_Initialize();
        if (MH_CreateHook(target, reinterpret_cast<void*>(&CJsonGetFloatHook),
                          reinterpret_cast<void**>(&s_originalCJsonGetFloat)) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            s_hook_installed = true;
            Log("CJsonGetFloat hook installed for arena timing overrides");
            if (s_config.round_time >= 0.0f)
                Log("  round_time: %.0f", s_config.round_time);
            if (s_config.point_score_celebration_time >= 0.0f)
                Log("  point_score_celebration_time: %.1f", s_config.point_score_celebration_time);
            if (s_config.mercy_win_point_spread >= 0.0f)
                Log("  mercy_win_point_spread: %.0f", s_config.mercy_win_point_spread);
        } else {
            Log("WARN: failed to create CJsonGetFloat hook");
        }
    }
#endif

    Log("initialization complete");
    return 0;
}

NEVR_PLUGIN_API void NvrPluginOnGameStateChange(const NvrGameContext* ctx,
                                                 uint32_t old_state,
                                                 uint32_t new_state) {
    (void)old_state;
    (void)new_state;

    /* Deferred balance config patching — retry until the struct is allocated */
    if (!s_overrides_applied && s_config.valid) {
        if (ApplyOverrides(s_base_addr, s_config)) {
            s_overrides_applied = true;
            Log("balance config overrides applied (deferred to state %u)", new_state);
        }
    }
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    Log("shutting down");
#ifdef _WIN32
    if (s_hook_installed) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        s_hook_installed = false;
    }
#endif
    s_config = {};
}

/* --------------------------------------------------------------------
 * DllMain for standalone injection mode (Windows only)
 * -------------------------------------------------------------------- */

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)hModule;
    (void)lpReserved;
    /*
     * Do NOT call NvrPluginInit/NvrPluginShutdown here.
     * The plugin loader calls LoadLibraryA (which triggers DllMain) and then
     * calls NvrPluginInit separately.  Auto-init from DllMain caused
     * double-initialization, crashes, and wrong timing.
     */
    if (reason == DLL_PROCESS_ATTACH) {
    } else if (reason == DLL_PROCESS_DETACH) {
    }
    return TRUE;
}
#endif

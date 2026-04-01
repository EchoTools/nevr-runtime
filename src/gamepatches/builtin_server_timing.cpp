/* SYNTHESIS -- custom tool code, not from binary */

/* ======================================================================
 * builtin_server_timing — Built-in version of the server-timing plugin
 *
 * Wine CPU optimization for EchoVR dedicated servers. Adapted from
 * plugins/server-timing/src/plugin.cpp into a built-in gamepatches module.
 *
 * Core patches:
 * 1. CPrecisionSleep::Wait hook — replaces WaitableTimer+BusyWait with Sleep(ms)
 * 2. SwitchToThread IAT patch — replaces sched_yield spin with Sleep(1)
 * 3. Delta time comparison patch — fixes signed JLE to unsigned JAE
 *
 * See plugins/server-timing/src/plugin.cpp for full problem analysis,
 * measurements, and risk assessment.
 * ====================================================================== */

#include "builtin_server_timing.h"
#include "patch_addresses.h"
#include "process_mem.h"
#include "common/logging.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <MinHook.h>
#endif

/* --------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------- */

struct ServerTimingConfig {
    uint32_t tick_rate_hz = 120;
    uint32_t tick_rate_idle_hz = 6;
    bool disable_busywait = true;
    bool fix_deltatime_comparison = true;
    bool fix_switchtothread = false;
    bool valid = false;
};

/* --------------------------------------------------------------------
 * Config file discovery — searches relative to game dir root,
 * not plugins/ subdir (unlike the plugin version).
 * -------------------------------------------------------------------- */

static std::string FindConfigFile() {
    const char* candidates[] = {
        "server_timing_config.yml",
        "server_timing_config.yaml",
        "server_timing_config.json",
    };

#ifdef _WIN32
    /* Search relative to the game executable directory */
    char exe_path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
        char* last_slash = std::strrchr(exe_path, '\\');
        if (!last_slash) last_slash = std::strrchr(exe_path, '/');
        if (last_slash) {
            *(last_slash + 1) = '\0';
            for (auto* name : candidates) {
                std::string path = std::string(exe_path) + name;
                FILE* f = std::fopen(path.c_str(), "r");
                if (f) { std::fclose(f); return path; }
            }
        }
    }
#endif

    /* Fallback: cwd */
    for (auto* name : candidates) {
        FILE* f = std::fopen(name, "r");
        if (f) { std::fclose(f); return name; }
    }
    return {};
}

/* --------------------------------------------------------------------
 * Minimal config parsers (YAML + JSON)
 * -------------------------------------------------------------------- */

static std::string ReadFileToString(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    if (sz <= 0) { std::fclose(f); return {}; }
    std::fseek(f, 0, SEEK_SET);
    std::string content(static_cast<size_t>(sz), '\0');
    std::fread(&content[0], 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    return content;
}

static bool ParseBool(const std::string& json, const char* key, bool default_val) {
    std::string search = std::string("\"") + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return default_val;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return default_val;
    auto rest = json.substr(pos + 1);
    size_t i = 0;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t' || rest[i] == '\n' || rest[i] == '\r'))
        ++i;
    if (rest.compare(i, 4, "true") == 0) return true;
    if (rest.compare(i, 5, "false") == 0) return false;
    return default_val;
}

static uint32_t ParseUint32(const std::string& json, const char* key, uint32_t default_val) {
    std::string search = std::string("\"") + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return default_val;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return default_val;
    auto rest = json.substr(pos + 1);
    size_t i = 0;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t' || rest[i] == '\n' || rest[i] == '\r'))
        ++i;
    if (i >= rest.size()) return default_val;
    char* end = nullptr;
    unsigned long val = std::strtoul(rest.c_str() + i, &end, 10);
    if (end == rest.c_str() + i) return default_val;
    return static_cast<uint32_t>(val);
}

/* Simple YAML key: value parser (scalar values only, no lists/objects) */
static bool YamlGetBool(const std::string& text, const char* key, bool default_val) {
    std::string needle = std::string(key) + ":";
    auto pos = text.find(needle);
    if (pos == std::string::npos) return default_val;
    auto rest = text.substr(pos + needle.size());
    size_t i = 0;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
    auto nl = rest.find('\n', i);
    std::string val = (nl != std::string::npos) ? rest.substr(i, nl - i) : rest.substr(i);
    /* Strip inline comment */
    auto hash = val.find('#');
    if (hash != std::string::npos) val = val.substr(0, hash);
    /* Trim trailing whitespace */
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r'))
        val.pop_back();
    if (val == "true" || val == "yes" || val == "on") return true;
    if (val == "false" || val == "no" || val == "off") return false;
    return default_val;
}

static uint32_t YamlGetUint32(const std::string& text, const char* key, uint32_t default_val) {
    std::string needle = std::string(key) + ":";
    auto pos = text.find(needle);
    if (pos == std::string::npos) return default_val;
    auto rest = text.substr(pos + needle.size());
    size_t i = 0;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
    if (i >= rest.size()) return default_val;
    char* end = nullptr;
    unsigned long val = std::strtoul(rest.c_str() + i, &end, 10);
    if (end == rest.c_str() + i) return default_val;
    return static_cast<uint32_t>(val);
}

static ServerTimingConfig ParseConfig(const std::string& text, bool is_yaml) {
    ServerTimingConfig cfg;
    if (is_yaml) {
        cfg.tick_rate_hz = YamlGetUint32(text, "tick_rate_hz", 120);
        cfg.tick_rate_idle_hz = YamlGetUint32(text, "tick_rate_idle_hz", 6);
        cfg.disable_busywait = YamlGetBool(text, "disable_busywait", true);
        cfg.fix_deltatime_comparison = YamlGetBool(text, "fix_deltatime_comparison", true);
        cfg.fix_switchtothread = YamlGetBool(text, "fix_switchtothread", false);
    } else {
        cfg.tick_rate_hz = ParseUint32(text, "tick_rate_hz", 120);
        cfg.tick_rate_idle_hz = ParseUint32(text, "tick_rate_idle_hz", 6);
        cfg.disable_busywait = ParseBool(text, "disable_busywait", true);
        cfg.fix_deltatime_comparison = ParseBool(text, "fix_deltatime_comparison", true);
        cfg.fix_switchtothread = ParseBool(text, "fix_switchtothread", false);
    }
    cfg.valid = true;
    return cfg;
}

/* --------------------------------------------------------------------
 * Memory patching
 * -------------------------------------------------------------------- */

#ifdef _WIN32
static bool PatchMemory(void* addr, const void* data, size_t len) {
    DWORD oldProtect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(addr, data, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    return true;
}
#endif

/* --------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------- */

static ServerTimingConfig g_config;
static uintptr_t g_base = 0;
static bool g_is_server = false;
static bool g_timestep_applied = false;
static bool g_wait_hook_installed = false;
static bool g_switchtothread_hook_installed = false;
static void* g_wait_hook_target = nullptr;

/* Resolve a virtual address from the PC binary to an in-process pointer */
static inline void* ResolveVA(uintptr_t base, uint64_t va) {
    return reinterpret_cast<void*>(base + (va - 0x140000000));
}

/* Virtual addresses from address_registry.h */
static constexpr uint64_t VA_PRECISION_SLEEP_WAIT     = 0x1401CE0B0;
static constexpr uint64_t VA_PRECISION_SLEEP_BUSYWAIT = 0x1401CE4C0;
static constexpr uint64_t VA_SWITCH_TO_THREAD         = 0x1401CE4B0;
static constexpr uint64_t VA_HEADLESS_DELTATIME       = 0x1400CF46D;

/* Game structure offsets (from patch_addresses.h) */
static constexpr uintptr_t GAME_TIMESTEP_FLAGS_OFFSET = PatchAddresses::GAME_TIMESTEP_FLAGS_OFFSET;
static constexpr uintptr_t FIXED_TIMESTEP_PTR         = PatchAddresses::FIXED_TIMESTEP_PTR;
static constexpr uintptr_t FIXED_TIMESTEP_OFFSET      = PatchAddresses::FIXED_TIMESTEP_OFFSET;

/* EchoVR NetGameState values */
static constexpr uint32_t STATE_LOBBY          = 5;
static constexpr uint32_t STATE_SERVER_LOADING = 6;

/* --------------------------------------------------------------------
 * CPrecisionSleep::Wait hook
 *
 * Replaces WaitableTimer + BusyWait with plain Sleep(ms).
 * On Wine 9.0 the original returns immediately, spinning the main
 * loop at 100% CPU. See plugin header for full analysis.
 * -------------------------------------------------------------------- */

#ifdef _WIN32
using PrecisionSleepWait_t = void(__fastcall*)(int64_t microseconds, int64_t unk, void* unk2);
static PrecisionSleepWait_t s_origWait = nullptr;

static int s_wait_trace_count = 0;

static void __fastcall PrecisionSleepWaitHook(int64_t microseconds, int64_t unk, void* unk2) {
    (void)unk;
    (void)unk2;
    if (s_wait_trace_count < 30) {
        Log(EchoVR::LogLevel::Debug, "[server_timing] Wait called: %lld us (%lld ms)",
            (long long)microseconds, (long long)(microseconds / 1000));
        s_wait_trace_count++;
    }
    if (microseconds > 0) {
        DWORD ms = static_cast<DWORD>(microseconds / 1000);
        if (ms < 1) ms = 1;
        Sleep(ms);
    } else {
        SwitchToThread();
    }
}

/* --------------------------------------------------------------------
 * SwitchToThread IAT patch
 *
 * The engine's task scheduler spin-polls SwitchToThread() in 10 yield/
 * wait/backoff paths. On Wine it maps to sched_yield() which returns
 * immediately. Replace via IAT overwrite with Sleep(1).
 * -------------------------------------------------------------------- */

using SwitchToThread_t = BOOL(WINAPI*)(void);
static SwitchToThread_t s_origSwitchToThread = nullptr;

static int s_stt_trace_count = 0;

static BOOL WINAPI SwitchToThreadHook(void) {
    if (s_stt_trace_count < 10) {
        Log(EchoVR::LogLevel::Debug, "[server_timing] SwitchToThread -> Sleep(1)");
        s_stt_trace_count++;
    }
    Sleep(1);
    return TRUE;
}
#endif

/* --------------------------------------------------------------------
 * Tick rate control
 * -------------------------------------------------------------------- */

static uint32_t g_current_tick_rate = 0;

static void SetTickRate(uint32_t hz) {
#ifdef _WIN32
    if (hz == 0 || hz == g_current_tick_rate) return;

    auto* timestep_ptr_base = *reinterpret_cast<char**>(g_base + FIXED_TIMESTEP_PTR);
    if (!timestep_ptr_base) return;

    auto* timestep = reinterpret_cast<uint32_t*>(timestep_ptr_base + FIXED_TIMESTEP_OFFSET);
    *timestep = 1000000 / hz;
    g_current_tick_rate = hz;

    Log(EchoVR::LogLevel::Info, "[server_timing] tick rate: %u Hz (%u us/tick)", hz, *timestep);
#else
    (void)hz;
#endif
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void BuiltinServerTiming::Init(uintptr_t base_addr, bool is_server) {
    if (!is_server) return;

    g_base = base_addr;
    g_is_server = true;

    Log(EchoVR::LogLevel::Info, "[server_timing] initializing (base=0x%llx)",
        static_cast<unsigned long long>(g_base));

    /* Load config */
    std::string config_path = FindConfigFile();
    if (config_path.empty()) {
        Log(EchoVR::LogLevel::Info, "[server_timing] no config file found, using defaults");
        g_config = ServerTimingConfig{};
        g_config.valid = true;
    } else {
        std::string content = ReadFileToString(config_path);
        if (content.empty()) {
            Log(EchoVR::LogLevel::Warning, "[server_timing] config file empty or unreadable, using defaults");
            g_config = ServerTimingConfig{};
            g_config.valid = true;
        } else {
            bool is_yaml = config_path.size() >= 4 &&
                (config_path.substr(config_path.size() - 4) == ".yml" ||
                 config_path.substr(config_path.size() - 5) == ".yaml");
            g_config = ParseConfig(content, is_yaml);
        }
    }

    Log(EchoVR::LogLevel::Info,
        "[server_timing] config: tick_rate_hz=%u tick_rate_idle_hz=%u "
        "disable_busywait=%s fix_deltatime=%s fix_switchtothread=%s",
        g_config.tick_rate_hz,
        g_config.tick_rate_idle_hz,
        g_config.disable_busywait ? "true" : "false",
        g_config.fix_deltatime_comparison ? "true" : "false",
        g_config.fix_switchtothread ? "true" : "false");

#ifdef _WIN32
    /* Hook CPrecisionSleep::Wait -> Sleep(ms) */
    if (g_config.disable_busywait) {
        g_wait_hook_target = ResolveVA(g_base, VA_PRECISION_SLEEP_WAIT);
        if (MH_CreateHook(g_wait_hook_target, reinterpret_cast<void*>(&PrecisionSleepWaitHook),
                          reinterpret_cast<void**>(&s_origWait)) == MH_OK &&
            MH_EnableHook(g_wait_hook_target) == MH_OK) {
            g_wait_hook_installed = true;
            Log(EchoVR::LogLevel::Info, "[server_timing] hooked CPrecisionSleep::Wait -> Sleep(ms)");
        } else {
            Log(EchoVR::LogLevel::Warning,
                "[server_timing] failed to hook CPrecisionSleep::Wait, falling back to BusyWait RET patch");
            void* busywait = ResolveVA(g_base, VA_PRECISION_SLEEP_BUSYWAIT);
            uint8_t ret = 0xC3;
            if (PatchMemory(busywait, &ret, 1)) {
                Log(EchoVR::LogLevel::Info, "[server_timing] patched CPrecisionSleep::BusyWait -> RET (fallback)");
            }
        }
    }

    /* Patch SwitchToThread IAT entry -> Sleep(1) wrapper.
       The thunk is rex.W jmp [rip+disp32]; parse disp32 to find IAT slot. */
    if (g_config.fix_switchtothread) {
        auto* thunk = reinterpret_cast<uint8_t*>(ResolveVA(g_base, VA_SWITCH_TO_THREAD));
        if (thunk[0] == 0x48 && thunk[1] == 0xFF && thunk[2] == 0x25) {
            int32_t disp = *reinterpret_cast<int32_t*>(thunk + 3);
            auto** iat_slot = reinterpret_cast<void**>(thunk + 7 + disp);
            s_origSwitchToThread = reinterpret_cast<SwitchToThread_t>(*iat_slot);
            void* hook_ptr = reinterpret_cast<void*>(&SwitchToThreadHook);
            if (PatchMemory(iat_slot, &hook_ptr, sizeof(void*))) {
                g_switchtothread_hook_installed = true;
                Log(EchoVR::LogLevel::Info,
                    "[server_timing] patched SwitchToThread IAT -> Sleep(1) (10 call sites)");
            } else {
                Log(EchoVR::LogLevel::Warning, "[server_timing] failed to patch SwitchToThread IAT slot");
            }
        } else {
            Log(EchoVR::LogLevel::Warning,
                "[server_timing] SwitchToThread thunk unexpected encoding: %02x %02x %02x",
                thunk[0], thunk[1], thunk[2]);
        }
    }
#endif

    Log(EchoVR::LogLevel::Info, "[server_timing] initialization complete");
}

void BuiltinServerTiming::OnFrame() {
    if (!g_is_server) return;

#ifdef _WIN32
    /* Hot-reload: check for tick_rate_override file every ~120 frames (~2s at 60Hz).
       Write a number (Hz) to this file to change tick rate live.
       Example: echo 60 > /opt/ready-at-dawn-echo-arena/bin/win10/tick_rate_override */
    static uint32_t frame_counter = 0;
    if (++frame_counter % 120 != 0) return;

    static std::string override_path;
    if (override_path.empty()) {
        char exe_path[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
            char* last_slash = std::strrchr(exe_path, '\\');
            if (!last_slash) last_slash = std::strrchr(exe_path, '/');
            if (last_slash) {
                *(last_slash + 1) = '\0';
                override_path = std::string(exe_path) + "tick_rate_override";
            }
        }
        if (override_path.empty()) override_path = "tick_rate_override";
    }

    FILE* f = std::fopen(override_path.c_str(), "r");
    if (!f) return;
    char buf[32] = {};
    std::fgets(buf, sizeof(buf), f);
    std::fclose(f);
    std::remove(override_path.c_str());  /* consume the file */

    uint32_t hz = static_cast<uint32_t>(std::strtoul(buf, nullptr, 10));
    if (hz >= 1 && hz <= 240) {
        SetTickRate(hz);
    }
#endif
}

void BuiltinServerTiming::OnGameStateChange(uint32_t old_state, uint32_t new_state) {
    if (!g_is_server) return;
    (void)old_state;

#ifdef _WIN32
    /* First state change: enable fixed timestep flag + delta time fix */
    if (!g_timestep_applied) {
        g_timestep_applied = true;

        if (g_config.fix_deltatime_comparison && g_config.tick_rate_hz > 0) {
            uint8_t patch[] = {0x73, 0x7A};  /* JLE -> JAE */
            void* addr = ResolveVA(g_base, VA_HEADLESS_DELTATIME);
            if (PatchMemory(addr, patch, sizeof(patch))) {
                Log(EchoVR::LogLevel::Info, "[server_timing] patched deltatime comparison (JLE -> JAE)");
            }
        }
    }

    /* Dynamic tick rate: lobby = idle rate, in-session = full rate */
    if (new_state == STATE_LOBBY) {
        SetTickRate(g_config.tick_rate_idle_hz);
    } else if (new_state >= STATE_SERVER_LOADING) {
        SetTickRate(g_config.tick_rate_hz);
    }
#else
    (void)new_state;
#endif
}

void BuiltinServerTiming::Shutdown() {
    if (!g_is_server) return;

    Log(EchoVR::LogLevel::Info, "[server_timing] shutting down");

#ifdef _WIN32
    if (g_wait_hook_installed && g_wait_hook_target) {
        MH_DisableHook(g_wait_hook_target);
        g_wait_hook_installed = false;
    }

    if (g_switchtothread_hook_installed && s_origSwitchToThread) {
        /* Restore original IAT entry */
        auto* thunk = reinterpret_cast<uint8_t*>(ResolveVA(g_base, VA_SWITCH_TO_THREAD));
        if (thunk[0] == 0x48 && thunk[1] == 0xFF && thunk[2] == 0x25) {
            int32_t disp = *reinterpret_cast<int32_t*>(thunk + 3);
            auto** iat_slot = reinterpret_cast<void**>(thunk + 7 + disp);
            void* orig = reinterpret_cast<void*>(s_origSwitchToThread);
            PatchMemory(iat_slot, &orig, sizeof(void*));
        }
        g_switchtothread_hook_installed = false;
    }
#endif
}

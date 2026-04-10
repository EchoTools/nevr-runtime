/* SYNTHESIS -- custom tool code, not from binary */

/* ======================================================================
 * server_timing — Wine CPU optimization for EchoVR dedicated servers
 *
 * PROBLEM
 * -------
 * EchoVR's engine has three Wine-hostile patterns that cause 91% CPU
 * usage on a 4-core server even when idle:
 *
 * 1. CPrecisionSleep::Wait (0x1401CE0B0) uses SetWaitableTimerEx(-1)
 *    + BusyWait. On Wine 9.0, the timer returns immediately and
 *    BusyWait spin-loops on SwitchToThread which doesn't yield.
 *    Result: main loop runs unthrottled at 100% of one core.
 *    FIXED: Hook Wait → Sleep(ms).
 *
 * 2. CPrecisionSleep::BusyWait (0x1401CE4C0) is a QPC + SwitchToThread
 *    spin loop for sub-ms frame timing precision. On Wine, SwitchToThread
 *    maps to sched_yield() which returns immediately on Linux.
 *    FIXED: Patch to RET (0xC3). We lose ~250µs precision per frame,
 *    negligible for a server.
 *
 * 3. CThreadPool::DrainWorkQueue (0x1401D66B0) — the task scheduler's
 *    "wait for workers to finish" function. After the main thread steals
 *    all available work items, it spin-polls SwitchToThread() at
 *    0x1401D67ED waiting for worker threads to complete. On Wine this
 *    is a tight spin. The same SwitchToThread thunk (0x1401CE4B0) is
 *    called from 10 sites — all are yield/wait/backoff paths:
 *
 *      0x1400CF4B0  spinlock backoff
 *      0x1400DB985  fence synchronization
 *      0x1401D3285  thread init wait
 *      0x1401D3877  SRWLock acquisition (physics, resources, audio)
 *      0x1401D67ED  DrainWorkQueue idle poll         ← hottest
 *      0x1401D798C  queue drain operation
 *      0x141338EF5  graphics fence wait (disabled on headless)
 *      0x141500F1A  spinlock with backoff
 *      0x14150D712  fiber scheduler
 *      0x14152846D  memory allocator contention
 *
 *    The physics pipeline calls the task flush (fcn_1401d64f0) 6 times
 *    per frame from OnSimulationAfter. Each flush enters DrainWorkQueue.
 *    At 30Hz with 10 worker threads, this means 6×11 threads×30fps of
 *    spin-polling = ~70% of CPU samples land in this code.
 *    TODO: Hook SwitchToThread thunk → Sleep(0).
 *
 * MEASUREMENTS (remote, 4-core Xeon E5-2680, Wine 9.0, headless)
 * ---------------------------------------------------------------
 *   Original (no patches):           91% CPU idle
 *   + Wait hook + BusyWait RET:      28% CPU idle (pre-match)
 *                                    37% CPU (1 player connected)
 *                                    41% CPU idle (post-match)
 *   + SwitchToThread hook (planned): expect ~5-10% idle
 *
 * WHY Sleep(1) FOR SwitchToThread
 * -------------------------------
 * Sleep(0) was tried first — on Wine it maps to sched_yield() which
 * returns immediately when no other thread wants the CPU (same problem
 * as SwitchToThread). Measured 194% CPU, worse than before the hook.
 *
 * Sleep(1) actually deschedules the thread for ~1ms. At 30Hz (33ms
 * frame budget) with 6 sync points per frame, worst case adds 6ms
 * latency, leaving 27ms for physics/network work. Acceptable for a
 * server that doesn't need sub-ms frame timing.
 *
 * RISK
 * ----
 * All 10 SwitchToThread call sites are in yield/wait/backoff paths,
 * not per-work-item hot paths. Adding sleep to a per-item dispatch
 * (our previous mistake at 0x1401D64F0) would compound — 1ms × 1000
 * items = 1 second. But these are idle-wait calls: one per drain cycle,
 * one per lock contention, one per fence check. Safe to sleep.
 *
 * MUST TEST UNDER GAMEPLAY LOAD, NOT JUST IDLE. (GUARDRAILS.md)
 * ====================================================================== */

#include "nevr_plugin_interface.h"
#include "nevr_common.h"
#include "address_registry.h"
#include "yaml_config.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <MinHook.h>
#endif

#ifndef NEVR_HOST_IS_HEADLESS
#define NEVR_HOST_IS_HEADLESS 0x10
#endif

/* --------------------------------------------------------------------
 * Logging
 * -------------------------------------------------------------------- */

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[server_timing] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

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

/* Game structure offsets (from nevr-runtime/src/gamepatches/patch_addresses.h) */
static constexpr uintptr_t GAME_TIMESTEP_FLAGS_OFFSET = 2088;
static constexpr uintptr_t FIXED_TIMESTEP_PTR = 0x020A00E8;
static constexpr uintptr_t FIXED_TIMESTEP_OFFSET = 0x90;

/* --------------------------------------------------------------------
 * Config file discovery
 * -------------------------------------------------------------------- */

static std::string FindConfigFile() {
    const char* candidates[] = {
        "server_timing_config.yml",
        "server_timing_config.yaml",
        "server_timing_config.json",
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

/* --------------------------------------------------------------------
 * Minimal JSON config parser
 * -------------------------------------------------------------------- */

static bool ParseBool(const std::string& json, const char* key, bool default_val) {
    std::string search = std::string("\"") + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return default_val;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return default_val;
    auto rest = json.substr(pos + 1);
    /* Skip whitespace */
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

static ServerTimingConfig ParseConfig(const std::string& text, bool is_yaml) {
    if (is_yaml) {
        YamlConfig y = ParseYamlConfig(text);
        ServerTimingConfig cfg;
        if (y.count("tick_rate_hz"))           cfg.tick_rate_hz = y["tick_rate_hz"].as_uint32(120);
        if (y.count("tick_rate_idle_hz"))      cfg.tick_rate_idle_hz = y["tick_rate_idle_hz"].as_uint32(6);
        if (y.count("disable_busywait"))       cfg.disable_busywait = y["disable_busywait"].as_bool(true);
        if (y.count("fix_deltatime_comparison")) cfg.fix_deltatime_comparison = y["fix_deltatime_comparison"].as_bool(true);
        if (y.count("fix_switchtothread"))     cfg.fix_switchtothread = y["fix_switchtothread"].as_bool(false);
        cfg.valid = true;
        return cfg;
    }
    /* JSON fallback */
    ServerTimingConfig cfg;
    cfg.tick_rate_hz = ParseUint32(text, "tick_rate_hz", 120);
    cfg.tick_rate_idle_hz = ParseUint32(text, "tick_rate_idle_hz", 6);
    cfg.disable_busywait = ParseBool(text, "disable_busywait", true);
    cfg.fix_deltatime_comparison = ParseBool(text, "fix_deltatime_comparison", true);
    cfg.fix_switchtothread = ParseBool(text, "fix_switchtothread", false);
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
 * Plugin state
 * -------------------------------------------------------------------- */

static ServerTimingConfig g_config;
static uintptr_t g_base = 0;
static bool g_initialized = false;
static bool g_timestep_applied = false;
static bool g_wait_hook_installed = false;
static bool g_switchtothread_hook_installed = false;

/* --------------------------------------------------------------------
 * CPrecisionSleep::Wait hook — replace with plain Sleep on Wine
 *
 * The original uses WaitableTimer + BusyWait. On Wine 9.0:
 * - WaitableTimer with SetWaitableTimerEx(-1) returns immediately
 * - BusyWait (already patched to RET) doesn't spin
 * - Result: the entire Wait function does nothing, loop runs unthrottled
 *
 * Fix: convert microseconds to milliseconds and call Sleep().
 * -------------------------------------------------------------------- */

#ifdef _WIN32
using PrecisionSleepWait_t = void(__fastcall*)(int64_t microseconds, int64_t unk, void* unk2);
static PrecisionSleepWait_t s_origWait = nullptr;

static int s_wait_trace_count = 0;

static void __fastcall PrecisionSleepWaitHook(int64_t microseconds, int64_t unk, void* unk2) {
    (void)unk;
    (void)unk2;
    if (s_wait_trace_count < 30) {
        Log("Wait called: %lld us (%lld ms)", (long long)microseconds, (long long)(microseconds / 1000));
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
 * SwitchToThread hook — fix task scheduler spin-wait on Wine
 *
 * The engine's task scheduler (CThreadPool::DrainWorkQueue at
 * 0x1401D66B0) spin-polls SwitchToThread() while waiting for worker
 * threads to finish. On Windows, SwitchToThread yields the timeslice
 * to another ready thread. On Wine, it maps to sched_yield() which
 * returns immediately on Linux — creating a tight spin loop.
 *
 * The SwitchToThread IAT thunk at 0x1401CE4B0 is called from exactly
 * 10 sites, all yield/wait/backoff paths (none per-work-item):
 *   - DrainWorkQueue idle poll (hottest — 6×/frame × 11 threads)
 *   - SRWLock acquisition contention
 *   - Spinlock backoff, fence waits, fiber scheduler, allocator
 *
 * Fix: hook the thunk → Sleep(0). Sleep(0) yields to any ready thread
 * of equal or higher priority (same intent as SwitchToThread) but
 * actually deschedules on Wine/Linux.
 *
 * See: nevr-runtime-plugins/docs/server-hardening/07-timing-and-threading.md
 * -------------------------------------------------------------------- */

using SwitchToThread_t = BOOL(WINAPI*)(void);
static SwitchToThread_t s_origSwitchToThread = nullptr;

static int s_stt_trace_count = 0;

static BOOL WINAPI SwitchToThreadHook(void) {
    if (s_stt_trace_count < 10) {
        Log("SwitchToThread -> Sleep(1)");
        s_stt_trace_count++;
    }
    /* Sleep(0) is insufficient on Wine — it maps to sched_yield() which
       returns immediately when no other thread wants the CPU, same as
       SwitchToThread. Sleep(1) actually deschedules for ~1ms.
       At 30Hz (33ms frame budget) with 6 sync points per frame, worst
       case adds 6ms latency — leaves 27ms for physics/network work. */
    Sleep(1);
    return TRUE;
}
#endif

/* --------------------------------------------------------------------
 * Plugin interface exports
 * -------------------------------------------------------------------- */

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "server_timing";
    info.description = "Server timing: frame pacing, tick rate, delta time fix";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
    return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    /* Server-only plugin */
    if (!(ctx->flags & NEVR_HOST_IS_SERVER)) {
        Log("not a server, skipping");
        return 0;
    }

    g_base = ctx->base_addr;
    Log("initializing (base=0x%llx)", static_cast<unsigned long long>(g_base));

    /* Load config */
    std::string config_path = FindConfigFile();
    if (config_path.empty()) {
        Log("no config file found, using defaults");
        g_config = ServerTimingConfig{};
        g_config.valid = true;
    } else {
        std::string content = nevr::LoadConfigFile(config_path);
        if (content.empty()) {
            Log("config file empty or unreadable, using defaults");
            g_config = ServerTimingConfig{};
            g_config.valid = true;
        } else {
            bool is_yaml = config_path.size() >= 4 &&
                (config_path.substr(config_path.size() - 4) == ".yml" ||
                 config_path.substr(config_path.size() - 5) == ".yaml");
            g_config = ParseConfig(content, is_yaml);
        }
    }

    Log("config: tick_rate_hz=%u tick_rate_idle_hz=%u disable_busywait=%s fix_deltatime=%s fix_switchtothread=%s",
        g_config.tick_rate_hz,
        g_config.tick_rate_idle_hz,
        g_config.disable_busywait ? "true" : "false",
        g_config.fix_deltatime_comparison ? "true" : "false",
        g_config.fix_switchtothread ? "true" : "false");

#ifdef _WIN32
    /* Replace CPrecisionSleep::Wait with plain Sleep() on Wine servers.
       The original's WaitableTimer doesn't sleep properly on Wine 9.0,
       causing the main loop to spin at 100% CPU. */
    if (g_config.disable_busywait) {
        void* wait_fn = nevr::ResolveVA(g_base, nevr::addresses::VA_PRECISION_SLEEP_WAIT);
        MH_Initialize();
        if (MH_CreateHook(wait_fn, reinterpret_cast<void*>(&PrecisionSleepWaitHook),
                          reinterpret_cast<void**>(&s_origWait)) == MH_OK &&
            MH_EnableHook(wait_fn) == MH_OK) {
            g_wait_hook_installed = true;
            Log("hooked CPrecisionSleep::Wait -> Sleep(ms)");
        } else {
            Log("WARN: failed to hook CPrecisionSleep::Wait, falling back to BusyWait RET patch");
            /* Fall back to just patching BusyWait to RET */
            void* busywait = nevr::ResolveVA(g_base, nevr::addresses::VA_PRECISION_SLEEP_BUSYWAIT);
            uint8_t ret = 0xC3;
            if (PatchMemory(busywait, &ret, 1)) {
                Log("patched CPrecisionSleep::BusyWait -> RET (fallback)");
            }
        }
    }

    /* Patch SwitchToThread IAT entry → our Sleep(1) wrapper.
       The thunk at VA_SWITCH_TO_THREAD is `jmp [rip+offset]` pointing to
       an IAT slot. MinHook can't hook a 7-byte JMP thunk reliably, so we
       overwrite the IAT pointer directly.
       See file header and docs/server-hardening/07-timing-and-threading.md */
    if (g_config.fix_switchtothread) {
        /* The thunk is: FF 25 <rel32> = jmp QWORD PTR [rip + rel32]
           Parse the rel32 to find the IAT slot address. */
        auto* thunk = reinterpret_cast<uint8_t*>(
            nevr::ResolveVA(g_base, nevr::addresses::VA_SWITCH_TO_THREAD));
        if (thunk[0] == 0x48 && thunk[1] == 0xFF && thunk[2] == 0x25) {
            /* rex.W jmp [rip+disp32] — disp32 is at thunk+3, RIP is thunk+7 */
            int32_t disp = *reinterpret_cast<int32_t*>(thunk + 3);
            auto** iat_slot = reinterpret_cast<void**>(thunk + 7 + disp);
            s_origSwitchToThread = reinterpret_cast<SwitchToThread_t>(*iat_slot);
            void* hook_ptr = reinterpret_cast<void*>(&SwitchToThreadHook);
            if (PatchMemory(iat_slot, &hook_ptr, sizeof(void*))) {
                g_switchtothread_hook_installed = true;
                Log("patched SwitchToThread IAT -> Sleep(1) (10 call sites, all yield/wait paths)");
            } else {
                Log("WARN: failed to patch SwitchToThread IAT slot");
            }
        } else {
            Log("WARN: SwitchToThread thunk has unexpected encoding: %02x %02x %02x",
                thunk[0], thunk[1], thunk[2]);
        }
    }
#endif

    g_initialized = true;
    Log("initialization complete");
    return 0;
}

/* EchoVR NetGameState values (from echovr.h) */
static constexpr uint32_t STATE_LOBBY = 5;
static constexpr uint32_t STATE_SERVER_LOADING = 6;
static constexpr uint32_t STATE_LOADING_LEVEL = 7;
static constexpr uint32_t STATE_READY_FOR_GAME = 8;
static constexpr uint32_t STATE_IN_GAME = 9;

static uint32_t g_current_tick_rate = 0;

static void SetTickRate(uint32_t hz) {
#ifdef _WIN32
    if (hz == 0 || hz == g_current_tick_rate) return;

    auto* timestep_ptr_base = *reinterpret_cast<char**>(g_base + FIXED_TIMESTEP_PTR);
    if (!timestep_ptr_base) return;

    auto* timestep = reinterpret_cast<uint32_t*>(timestep_ptr_base + FIXED_TIMESTEP_OFFSET);
    *timestep = 1000000 / hz;
    g_current_tick_rate = hz;

    Log("tick rate: %u Hz (%u us/tick)", hz, *timestep);
#else
    (void)hz;
#endif
}

NEVR_PLUGIN_API void NvrPluginOnGameStateChange(const NvrGameContext* ctx,
                                                 uint32_t old_state,
                                                 uint32_t new_state) {
    (void)old_state;
    if (!g_initialized) return;

#ifdef _WIN32
    /* First call: enable fixed timestep flag + delta time fix */
    if (!g_timestep_applied) {
        g_timestep_applied = true;

        if (g_config.tick_rate_hz > 0 && ctx->net_game != nullptr) {
            auto* flags_ptr = reinterpret_cast<uint64_t*>(
                reinterpret_cast<char*>(ctx->net_game) + GAME_TIMESTEP_FLAGS_OFFSET);
            *flags_ptr |= 0x2000000;
            Log("fixed timestep flag set");
        }

        if (g_config.fix_deltatime_comparison && g_config.tick_rate_hz > 0) {
            uint8_t patch[] = {0x73, 0x7A};
            void* addr = nevr::ResolveVA(g_base, nevr::addresses::VA_HEADLESS_DELTATIME);
            if (PatchMemory(addr, patch, sizeof(patch))) {
                Log("patched deltatime comparison (JLE -> JAE)");
            }
        }
    }

    /* Dynamic tick rate based on game state.
       Lobby = no active session, idle tick rate (default 6Hz).
       ServerLoading or later = session assigned, full tick rate.
       Back to lobby after match = drop to idle again. */
    if (new_state == STATE_LOBBY) {
        SetTickRate(g_config.tick_rate_idle_hz);
    } else if (new_state >= STATE_SERVER_LOADING) {
        SetTickRate(g_config.tick_rate_hz);
    }
#else
    (void)ctx;
    (void)new_state;
#endif
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext* ctx) {
    (void)ctx;
    if (!g_initialized) return;
#ifdef _WIN32
    /* Hot-reload: check for tick_rate_override file every ~2 seconds.
       Write a number (Hz) to this file to change tick rate live.
       SSH example: echo 60 > /opt/ready-at-dawn-echo-arena/bin/win10/plugins/tick_rate_override */
    static uint32_t frame_counter = 0;
    if (++frame_counter % 120 != 0) return;  /* ~2s at 60Hz, ~1s at 120Hz */

    static std::string override_path;
    if (override_path.empty()) {
        char dll_path[MAX_PATH] = {};
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&NvrPluginOnFrame), &hm)) {
            GetModuleFileNameA(hm, dll_path, MAX_PATH);
            char* last_slash = std::strrchr(dll_path, '\\');
            if (!last_slash) last_slash = std::strrchr(dll_path, '/');
            if (last_slash) {
                *(last_slash + 1) = '\0';
                override_path = std::string(dll_path) + "tick_rate_override";
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

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    Log("shutting down");
#ifdef _WIN32
    if (g_wait_hook_installed) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
#endif
}

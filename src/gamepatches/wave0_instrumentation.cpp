/* SYNTHESIS -- custom tool code, not from binary */

/* ======================================================================
 * wave0_instrumentation — Bug fix hooks for verified binary bugs
 *
 * Hooks:
 * 0a. GetTimeMicroseconds — overflow-safe replacement (BUG #1, Critical)
 *     Original: (perfCount * 1000000) / perfFreq overflows INT64_MAX
 *     after ~10.68 days at 10MHz QPC or ~4.3 days at 25MHz QPC.
 *     Fix: split into quotient + remainder to avoid intermediate overflow.
 *     This also prevents BUG #2 (CleanupPeers mass disconnect) since
 *     bad timestamps never propagate.
 *
 * 0b. CTimer_GetMilliSeconds — overflow-safe replacement (BUG #2, High)
 *     Original: computes µs first (×10^6) then ÷1000 — same intermediate
 *     overflow as BUG #1 at ~10.68 days. CleanupPeers (the BUG #2 site)
 *     calls this function, so the µs fix alone does NOT prevent BUG #2.
 *     Fix: direct ms computation via quotient+remainder split to avoid
 *     intermediate overflow. Does NOT call original.
 *
 * 0c. EndMultiplayer — null deref prevention (BUG #6, High)
 *     Check pointer at arg1+0x2DA0 before double-deref that crashes.
 *
 * 0d. CPrecisionSleep::Wait — cached high-res timer (BUG #11, #12, High)
 *     Original: creates/destroys kernel timer each frame (180 kernel
 *     transitions/sec at 90fps) with standard-res timer. Fix: persistent
 *     high-res waitable timer created once at init.
 *
 * 0e. CSpinWait::WaitForValue — corrected backoff (BUG #14, High)
 *     Original: sleep_ms starts at 10 and decrements to 0 (maximum CPU
 *     at peak contention). Fix: start at 0, increment to 10 (proper
 *     increasing backoff). Adds YieldProcessor() for HT friendliness.
 *
 * 0f. CPrecisionSleep::BusyWait — RET patch (BUG #13, High)
 *     Patches first byte to 0xC3 (RET). Eliminates tight QPC+SwitchToThread
 *     spin loop. Only loses ~250us of busy-wait precision per frame.
 * ====================================================================== */

#include "wave0_instrumentation.h"
#include "mode_patches.h"
#include "hook_liveness.h"
#include "builtin_log_filter.h"
#include "patch_addresses.h"
#include "common/globals.h"
#include "common/logging.h"
#include "plugin_loader.h"    // N68: TickPlugins
#include "module_loader.h"    // N68: TickModules

#ifdef _WIN32
#include <windows.h>
#include <cstring>
#include <string>
#include <MinHook.h>
#include "process_mem.h"
#include "cli.h"              // g_isServer
#include "crash_recovery.h"   // ForceFatalExit, PerformGracefulShutdown
#endif

/* --------------------------------------------------------------------
 * Address constants (full VAs for ResolveVA)
 * -------------------------------------------------------------------- */

static constexpr uint64_t VA_GET_TIME_MICROSECONDS   = 0x1400D00C0;
static constexpr uint64_t VA_GET_TIME_MILLISECONDS   = 0x1400D0110;
static constexpr uint64_t VA_END_MULTIPLAYER         = 0x140162450;
static constexpr uint64_t VA_PRECISION_SLEEP_WAIT    = 0x1401CE0B0;
static constexpr uint64_t VA_PRECISION_SLEEP_BUSYWAIT = 0x1401CE4C0;
static constexpr uint64_t VA_SPINWAIT_WAIT_FOR_VALUE = 0x141500ED8;
static constexpr uint64_t VA_HTTP_LISTENER_BRINGUP   = 0x1401F5B00;  // BUG #62

// N33: save original byte at BusyWait before RET patch, restore on Shutdown.
static uint8_t  s_busywait_original_byte = 0;
static bool     s_busywait_patched       = false;

/* Expected prologue at VA_HTTP_LISTENER_BRINGUP: MOV [RSP+8],RBX (relocatable,
 * clean 5-byte hook boundary). Validated before hooking to guard against the
 * loaded binary diverging from the ReVault-indexed echovr.exe. */
static constexpr uint8_t HTTP_LISTENER_PROLOGUE[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};

/* Expected prologue at VA_GET_TIME_MICROSECONDS (0x1400D00C0): SUB RSP,0x28
 * Same prologue at VA_GET_TIME_MILLISECONDS (0x1400D0110). Both ReVault-verified. */
static constexpr uint8_t GET_TIME_MICROSECONDS_PROLOGUE[4] = {0x48, 0x83, 0xEC, 0x28};
static constexpr uint8_t GET_TIME_MILLISECONDS_PROLOGUE[4] = {0x48, 0x83, 0xEC, 0x28};

/* Expected prologue at VA_END_MULTIPLAYER (0x140162450): MOV [RSP+0x10],RBX
 * ReVault-verified: 0x140162450: 48 89 5c 24 10. */
static constexpr uint8_t END_MULTIPLAYER_PROLOGUE[5] = {0x48, 0x89, 0x5C, 0x24, 0x10};

/* CSpinWait::WaitForValue global spin limit offset (from ImageBase) */
static constexpr uintptr_t OFF_SPINWAIT_SPIN_LIMIT   = 0x2034500;

/* GetTimeMicroseconds global data offsets (from ImageBase 0x140000000) */
static constexpr uintptr_t OFF_TIME_OVERRIDE_FLAG  = 0x2099038;  // int64, non-zero = use cached value
static constexpr uintptr_t OFF_TIME_OVERRIDE_VALUE = 0x209CB00;  // uint64, cached microsecond value

/* --------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------- */

static uintptr_t g_base = 0;
static bool g_initialized = false;
static int64_t s_cached_perf_freq = 0;  // QPC frequency, constant per process

#ifdef _WIN32
static HANDLE s_cached_timer = NULL;    // Persistent waitable timer for frame pacer

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

/* Hot-path address resolution — targets validated at init time */
static inline void* ResolveVA(uintptr_t base, uint64_t va) {
    return reinterpret_cast<void*>(base + (va - 0x140000000));
}

/* Validated resolution for init-time setup — returns nullptr on bad address */
static inline void* ResolveVA_Safe(uintptr_t base, uint64_t va) {
    if (va < 0x140000000ULL) return nullptr;
    void* addr = reinterpret_cast<void*>(base + (va - 0x140000000));
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return nullptr;
    if (mbi.State != MEM_COMMIT) return nullptr;
#endif
    return addr;
}

#ifdef _WIN32

/* --------------------------------------------------------------------
 * Hook 0a — GetTimeMicroseconds overflow FIX (BUG #1, Critical)
 *
 * Original: return (perfCount * 1000000) / perfFreq
 * The multiplication overflows INT64_MAX after ~10.68 days at 10MHz
 * QPC or ~4.3 days at 25MHz (Ryzen). Wraps negative, producing
 * garbage timestamps that cascade to BUG #2 (CleanupPeers mass
 * disconnect via elapsed time wraparound).
 *
 * Fix: split into quotient + remainder to avoid intermediate overflow.
 *   (a * b) / c  =  (a / c) * b  +  ((a % c) * b) / c
 * where a = perfCount, b = 1000000, c = perfFreq.
 * Since perfFreq is typically 10-25MHz, (a % c) * 1000000 is at most
 * ~25 * 10^12, well within int64 range (~9.2 * 10^18).
 *
 * Does NOT call original — replaces computation entirely.
 * Replicates the global override check for paused/fixed time.
 * -------------------------------------------------------------------- */

using GetTimeMicroseconds_t = uint64_t(__fastcall*)();
static GetTimeMicroseconds_t s_origGetTimeMicroseconds = nullptr;

// ---------------------------------------------------------------------------
// N86 — the per-frame tick site that actually runs on a dedicated server
// ---------------------------------------------------------------------------
//
// N68 wired TickPlugins/TickModules into PrecisionSleepWaitHook and closed on
// `just verify` green. That proved the CALL SITE existed in source; it did not
// prove the site ever executes. Measured on a live headless server:
// CPrecisionSleep::Wait is hooked successfully ("hooks installed: 7 succeeded,
// 0 failed") and then called ZERO times for the entire run. Plugins never got
// OnFrame, modules never got their tick, and N69's per-thread stack reserve was
// never claimed on any game thread.
//
// GetTimeMicroseconds IS live in server mode (measured, same run). It is far
// hotter than one call per frame, so the dispatch is rate-limited by elapsed
// time using this function's OWN return value — no extra clock, and it tracks
// real time rather than a call count that varies with engine load.
static volatile LONG g_tickReentry = 0;
static uint64_t g_lastTickUs = 0;

static constexpr uint64_t kTickIntervalUs = 8000;  // ~125 Hz, the frame-pacer cadence

/// Dispatch per-frame work from a site known to run. Re-entrancy is guarded
/// because plugin/module OnFrame handlers may themselves call anything that
/// calls GetTimeMicroseconds — without the gate that is unbounded recursion.
static void DispatchPerFrameWork(uint64_t nowUs) {
    if (nowUs - g_lastTickUs < kTickIntervalUs) return;
    if (InterlockedExchange(&g_tickReentry, 1) != 0) return;  // already inside
    g_lastTickUs = nowUs;

    EnsureStackReserve();  // N69: covers whatever thread drives the loop
    BuiltinLogFilter::InstallPnsradHook();  // N90: idempotent; installs once pnsrad.dll loads
    BuiltinLogFilter::PollHealth();         // N89: health must not depend on the hook it watches

    // Liveness + N83/N84 evidence, ~every 30s (3750 ticks x 8ms).
    {
        static volatile LONG s_ticks = 0;
        const LONG t = InterlockedIncrement(&s_ticks);
        if (t == 1) {
            Log(EchoVR::LogLevel::Info,
                "[NEVR.PATCH] per-frame tick ALIVE via GetTimeMicroseconds (N86) — "
                "plugin/module OnFrame now dispatched in server mode");
        }
        if ((t % 3750) == 0) {
            LogBroadcasterHookStats();
            // N86-class standing check: name every hook that installed and has
            // never been entered. This is the measurement whose absence let a
            // dead per-frame tick ship for a day.
            HookLiveness::Report("periodic");
        }
    }

    NvrGameContext gctx = {};
    gctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
    gctx.flags = g_isServer ? NEVR_HOST_IS_SERVER : NEVR_HOST_IS_CLIENT;
    if (g_isHeadless) gctx.flags |= NEVR_HOST_IS_HEADLESS;
    TickPlugins(&gctx);

    NvrModuleContext mctx = {};
    mctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
    mctx.flags = gctx.flags;
    TickModules(&mctx);

    InterlockedExchange(&g_tickReentry, 0);
}

static uint64_t __fastcall GetTimeMicrosecondsHook() {
    HookLiveness::Mark(HookLiveness::kGetTimeMicroseconds);
    // Replicate the original's global override check.
    // When the engine pauses or fixes time, it sets a flag and cached value.
    volatile int64_t* override_flag = reinterpret_cast<volatile int64_t*>(
        g_base + OFF_TIME_OVERRIDE_FLAG);
    if (*override_flag != 0) {
        return *reinterpret_cast<volatile uint64_t*>(
            g_base + OFF_TIME_OVERRIDE_VALUE);
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    int64_t pc = counter.QuadPart;
    int64_t pf = s_cached_perf_freq;

    // Overflow-safe: (pc * 1000000) / pf without intermediate overflow
    int64_t whole = (pc / pf) * 1000000LL;
    int64_t frac  = ((pc % pf) * 1000000LL) / pf;
    const uint64_t nowUs = static_cast<uint64_t>(whole + frac);

    // N86: drive per-frame work from here — this site is live in server mode,
    // PrecisionSleep::Wait is not. Rate-limited and re-entrancy-guarded inside.
    DispatchPerFrameWork(nowUs);

    return nowUs;
}

/* --------------------------------------------------------------------
 * Hook 0b — CTimer_GetMilliSeconds overflow-safe replacement (BUG #2, High)
 *
 * Original fcn.1400d0110 (ReVault-verified): computes microseconds first
 *   (perfCount * 1000000) / perfFreq, then divides by 1000. The x10^6
 *   intermediate overflows INT64_MAX after ~10.68 days at 10MHz QPC
 *   (~4.3 days at 25MHz) — same overflow as BUG #1.
 *
 * 8 callers including CleanupPeers @ 0x140F76500 (the BUG #2 mass
 * peer-disconnect site). The GetTimeMicroseconds fix (0a) does NOT
 * prevent BUG #2 because CleanupPeers calls this ms variant, not the
 * us variant — so BUG #2 was unmitigated until this fix.
 *
 * Fix: split into quotient + remainder to avoid intermediate overflow.
 *   (a * 1000) / c  =  (a / c) * 1000  +  ((a % c) * 1000) / c
 * Since perfFreq is typically 10-25MHz, (a % c) * 1000 is at most
 * ~25 * 10^9, well within int64 range (~9.2 * 10^18).
 *
 * Does NOT call original — replaces computation entirely.
 * Replicates the global override check for paused/fixed time.
 * -------------------------------------------------------------------- */

using GetTimeMilliseconds_t = uint64_t(__fastcall*)();
static GetTimeMilliseconds_t s_origGetTimeMilliseconds = nullptr;

static uint64_t __fastcall GetTimeMillisecondsHook() {
    HookLiveness::Mark(HookLiveness::kGetTimeMilliseconds);
    // Replicate the original's global override check.
    // When time is paused/fixed, the override stores microseconds.
    // CTimer_GetMilliSeconds returns that cached us value / 1000.
    volatile int64_t* override_flag = reinterpret_cast<volatile int64_t*>(
        g_base + OFF_TIME_OVERRIDE_FLAG);
    if (*override_flag != 0) {
        uint64_t us = *reinterpret_cast<volatile uint64_t*>(
            g_base + OFF_TIME_OVERRIDE_VALUE);
        return us / 1000;  // ms from cached us
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    int64_t pc = counter.QuadPart;
    int64_t pf = s_cached_perf_freq;

    // Overflow-safe: (pc * 1000) / pf without intermediate overflow
    int64_t whole = (pc / pf) * 1000LL;
    int64_t frac  = ((pc % pf) * 1000LL) / pf;

    return static_cast<uint64_t>(whole + frac);
}

/* --------------------------------------------------------------------
 * Hook 0c — EndMultiplayer null deref detection
 *
 * Check the pointer at arg1 + 0x2DA0 before the double-deref that
 * crashes. Returns early to prevent the crash when NULL.
 * -------------------------------------------------------------------- */

using EndMultiplayer_t = void(__fastcall*)(int64_t arg1, int64_t arg2);
static EndMultiplayer_t s_origEndMultiplayer = nullptr;
static volatile LONG s_null_deref_count = 0;

static void __fastcall EndMultiplayerHook(int64_t arg1, int64_t arg2) {
    HookLiveness::Mark(HookLiveness::kEndMultiplayer);
    if (arg1 != 0) {
        int64_t* session_ptr = reinterpret_cast<int64_t*>(arg1 + 0x2DA0);
        if (*session_ptr == 0) {
            LONG count = InterlockedIncrement(&s_null_deref_count);
            Log(EchoVR::LogLevel::Warning,
                "[wave0] EndMultiplayer: session ptr at +0x2DA0 is NULL (crash prevented, count=%ld)",
                count);
            return;  // Skip the original — it would crash
        }
    }
    s_origEndMultiplayer(arg1, arg2);
}

/* --------------------------------------------------------------------
 * Hook 0d — CPrecisionSleep::Wait replacement (BUG #11, #12, High)
 *
 * Original creates and destroys a kernel timer handle every frame
 * (180 kernel transitions/sec at 90fps) and uses a standard timer
 * that can't accept CREATE_WAITABLE_TIMER_HIGH_RESOLUTION.
 *
 * Fix: use a persistent high-res waitable timer created once at init.
 * Timer precision improves from ~15.6ms to ~0.5ms on Windows 10 1803+.
 * Falls back to standard timer on older Windows.
 *
 * On servers, server_timing hooks this function later with WSAPoll-
 * based event-driven recv. That hook chains on top and never calls
 * this version (it replaces Wait entirely), so no conflict.
 * -------------------------------------------------------------------- */

using PrecisionSleepWait_t = void(__fastcall*)(int64_t microseconds, int64_t unk, void* unk2);
static PrecisionSleepWait_t s_origPrecisionSleepWait = nullptr;

static void __fastcall PrecisionSleepWaitHook(int64_t microseconds, int64_t unk, void* unk2) {
    HookLiveness::Mark(HookLiveness::kPrecisionSleepWait);
    (void)unk;
    (void)unk2;

    // N69: claim crash-handler stack reserve on whatever thread runs the frame
    // pacer. InstallVEH only covers the thread that called it; this covers every
    // game thread that reaches our per-frame hook. thread_local one-shot — after
    // the first call on a thread this is a single bool test.
    EnsureStackReserve();

    // N83/N84: periodic broadcaster-hook entry counts. ~every 30s at 8ms frames.
    {
        // N86: this hook (CPrecisionSleep::Wait) is NOT called in server mode —
        // measured 0 entries across a full run. Retained for client mode, where
        // it is the correct frame-pacer site. Server-mode dispatch happens in
        // DispatchPerFrameWork, driven by GetTimeMicroseconds.
        static volatile LONG s_frames = 0;
        InterlockedIncrement(&s_frames);
    }

    // Check for graceful shutdown request (set by SIGINT/SIGTERM handler).
    // This fires every game tick, so CTRL+C responsiveness is bounded by the
    // frame interval (~8ms at 120Hz, ~16ms at 60Hz).
    if (g_shutdownRequested) {
      PerformGracefulShutdown(0);
      // Unreachable — ForceFatalExit calls TerminateProcess.
    }

    // N68: tick plugin + module per-frame callbacks. The broadcaster-bridge
    // plugin registers NvrPluginOnFrame (plugin_main.cpp:43) and the plugin
    // API is documented (plugins/example/README.md), but these were never
    // called from any per-frame loop.
    {
      NvrGameContext gctx = {};
      gctx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
      gctx.flags = NEVR_HOST_IS_SERVER;  // always server at this point
      if (g_isHeadless) gctx.flags |= NEVR_HOST_IS_HEADLESS;
      TickPlugins(&gctx);

      NvrModuleContext mctx = {};
      mctx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
      mctx.flags = NEVR_HOST_IS_SERVER;
      TickModules(&mctx);
    }

    if (microseconds <= 0) {
        SwitchToThread();
        return;
    }

    if (s_cached_timer != NULL) {
        // High-precision wait using persistent timer handle.
        // Negative due_time = relative, in 100-nanosecond units.
        LARGE_INTEGER due_time;
        due_time.QuadPart = -(microseconds * 10);
        if (SetWaitableTimerEx(s_cached_timer, &due_time, 0, NULL, NULL, NULL, 0)) {
            DWORD timeout_ms = static_cast<DWORD>(microseconds / 1000) + 2;
            WaitForSingleObject(s_cached_timer, timeout_ms);
        }
    } else {
        // Fallback: millisecond-precision Sleep
        DWORD ms = static_cast<DWORD>(microseconds / 1000);
        if (ms < 1) ms = 1;
        Sleep(ms);
    }
}

/* --------------------------------------------------------------------
 * Hook 0e — CSpinWait::WaitForValue backoff fix (BUG #14, High)
 *
 * Original starts sleep_ms at 10 and decrements to 0 — maximum CPU
 * consumption at peak contention. Fix: start at 0 and increment to
 * 10 (proper increasing backoff). Also adds YieldProcessor() for
 * hyperthreading-friendly spinning.
 *
 * Signature: void __fastcall(uint32_t* ptr, uint32_t expected, uint32_t mask)
 * -------------------------------------------------------------------- */

using WaitForValue_t = void(__fastcall*)(volatile uint32_t* ptr, uint32_t expected, uint32_t mask);
static WaitForValue_t s_origWaitForValue = nullptr;

// Reasonable bounds for the game's native spin-limit global.
// The true value on echovr.exe 34 is ~4000; anything outside [100, 50000]
// indicates a version-drift silent misread (N31).
static constexpr uint32_t SPIN_LIMIT_MIN = 100;
static constexpr uint32_t SPIN_LIMIT_MAX = 50000;
static constexpr uint32_t SPIN_LIMIT_DEFAULT = 4000;

static void __fastcall WaitForValueHook(volatile uint32_t* ptr, uint32_t expected, uint32_t mask) {
    HookLiveness::Mark(HookLiveness::kSpinWaitForValue);
    // Read configurable spin limit from game's global data.
    // Validated against bounds to catch version-drift silent misread (N31).
    uint32_t spin_limit = *reinterpret_cast<volatile uint32_t*>(g_base + OFF_SPINWAIT_SPIN_LIMIT);
    if (spin_limit < SPIN_LIMIT_MIN || spin_limit > SPIN_LIMIT_MAX) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] WaitForValue spin_limit=%u out of bounds [%u,%u] — "
            "possible version drift at offset 0x%X, using default %u",
            spin_limit, SPIN_LIMIT_MIN, SPIN_LIMIT_MAX,
            static_cast<unsigned int>(OFF_SPINWAIT_SPIN_LIMIT), SPIN_LIMIT_DEFAULT);
        spin_limit = SPIN_LIMIT_DEFAULT;
    }

    uint32_t spin_count = 0;
    uint32_t sleep_ms = 0;  // Start at 0, increase under contention (not inverted)

    while ((*ptr & mask) != expected) {
        YieldProcessor();  // PAUSE instruction — yields to HT sibling
        spin_count++;
        if (spin_count >= spin_limit) {
            Sleep(sleep_ms == 0 ? 0 : 1);
            spin_count = 0;
            if (sleep_ms < 10) sleep_ms++;  // Increasing backoff
        }
    }
}

/* --------------------------------------------------------------------
 * Hook 0g — httpport HTTP-listener bind failure -> fatal (BUG #62, High)
 *
 * fcn.1401F5B00 is the game's HTTP API listener bring-up wrapper:
 *   uint64_t __fastcall(state, address, port) -> 1 on success, 0 on failure.
 * On a bind failure (port already in use) it frees the listener and returns 0,
 * but the caller (fcn.140157FB0) only logs "[NETGAME] Failed to bind HTTP
 * listener" and continues. The server then runs headless with a dead session
 * API — invisible to nevr-agent, so every match it hosts records ZERO tape
 * (silent, total data loss). Observed on gameserver-chi1 2026-06-28.
 *
 * Fix: in server mode, a 0 return is fatal. Force a real process exit (via
 * ForceFatalExit, which bypasses the server-mode ExitProcess suppression).
 * Client mode and the success path are passed through unchanged.
 * -------------------------------------------------------------------- */

using HttpListenerBringup_t = uint64_t(__fastcall*)(int64_t* state, const char* address, uint16_t port);
static HttpListenerBringup_t s_origHttpListenerBringup = nullptr;

static uint64_t __fastcall HttpListenerBringupHook(int64_t* state, const char* address, uint16_t port) {
    HookLiveness::Mark(HookLiveness::kHttpListenerBringup);
    uint64_t result = s_origHttpListenerBringup(state, address, port);
    if (result == 0 && g_isServer) {
        Log(EchoVR::LogLevel::Error,
            "[wave0] HTTP API listener failed to bind %s:%u (port in use). A server with "
            "no HTTP API records zero tape — silent data loss. Forcing fatal exit (BUG #62).",
            address ? address : "(null)", static_cast<unsigned>(port));
        ForceFatalExit(62);  // bypasses server-mode ExitProcess suppression
    }
    return result;  // success, or client mode — preserve original behavior
}

#endif  // _WIN32

/* ====================================================================
 * Public API
 * ==================================================================== */

void Wave0::Init(uintptr_t base_addr) {
    g_base = base_addr;

    if (g_initialized) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] wave0 already initialized, skipping duplicate Init");
        return;
    }

    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] wave0 installing bug fix hooks");

#ifdef _WIN32
    // Cache QPC frequency (constant per process, ~20ns on Windows, ~1-5us on Wine)
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    s_cached_perf_freq = freq.QuadPart;
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] wave0 qpc_freq=%lld Hz",
        static_cast<long long>(s_cached_perf_freq));

    // Create persistent high-res waitable timer (BUG #11, #12 fix)
    // Try high-res first (Windows 10 1803+), fall back to standard
    if (s_cached_timer != NULL) {
        CloseHandle(s_cached_timer);
        s_cached_timer = NULL;
    }
    s_cached_timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (s_cached_timer) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] wave0 timer=high_resolution");
    } else {
        s_cached_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (s_cached_timer) {
            Log(EchoVR::LogLevel::Info,
                "[NEVR.PATCH] wave0 timer=standard (high-res unavailable)");
        } else {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] wave0 timer=failed fallback=sleep");
        }
    }

    struct HookEntry {
        uint64_t va;
        void* detour;
        void** original;
        const char* name;
        const uint8_t* prologue;  // nullptr = skip prologue validation
        uint8_t prologue_len;     // byte count for prologue comparison (0 if no prologue)
    };

    HookEntry hooks[] = {
        { VA_GET_TIME_MICROSECONDS, (void*)&GetTimeMicrosecondsHook,
          (void**)&s_origGetTimeMicroseconds, "GetTimeMicroseconds (BUG#1 fix)",
          GET_TIME_MICROSECONDS_PROLOGUE, sizeof(GET_TIME_MICROSECONDS_PROLOGUE) },
        { VA_GET_TIME_MILLISECONDS, (void*)&GetTimeMillisecondsHook,
          (void**)&s_origGetTimeMilliseconds, "CTimer_GetMilliSeconds",
          GET_TIME_MILLISECONDS_PROLOGUE, sizeof(GET_TIME_MILLISECONDS_PROLOGUE) },
        { VA_END_MULTIPLAYER, (void*)&EndMultiplayerHook,
          (void**)&s_origEndMultiplayer, "EndMultiplayer (BUG#6 fix)",
          END_MULTIPLAYER_PROLOGUE, sizeof(END_MULTIPLAYER_PROLOGUE) },
        { VA_PRECISION_SLEEP_WAIT, (void*)&PrecisionSleepWaitHook,
          (void**)&s_origPrecisionSleepWait, "CPrecisionSleep::Wait (BUG#11/#12 fix)",
          nullptr, 0 },
        { VA_SPINWAIT_WAIT_FOR_VALUE, (void*)&WaitForValueHook,
          (void**)&s_origWaitForValue, "CSpinWait::WaitForValue (BUG#14 fix)",
          nullptr, 0 },
    };

    int installed = 0;
    int failed = 0;
    std::string failedNames;

    for (auto& h : hooks) {
        void* target = ResolveVA_Safe(g_base, h.va);
        if (!target) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] hook skipped name=%s va=0x%llX reason=address_unmapped",
                h.name, static_cast<unsigned long long>(h.va));
            if (!failedNames.empty()) failedNames += ", ";
            failedNames += h.name;
            failed++;
            continue;
        }

        // Prologue validation (N25/N28): verify expected bytes before hooking
        if (h.prologue && h.prologue_len > 0 &&
            memcmp(target, h.prologue, h.prologue_len) != 0) {
            // Read actual bytes for diagnostics
            uint8_t actual[4] = {0};
            memcpy(actual, target, sizeof(actual));
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] hook skipped name=%s va=0x%llX reason=prologue_mismatch "
                "expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x",
                h.name, static_cast<unsigned long long>(h.va),
                h.prologue[0], h.prologue[1], h.prologue[2], h.prologue[3],
                actual[0], actual[1], actual[2], actual[3]);
            if (!failedNames.empty()) failedNames += ", ";
            failedNames += h.name;
            failed++;
            continue;
        }

        // Read actual prologue bytes before hooking for Rule 5 failure reporting
        uint8_t actual[4] = {0};
        memcpy(actual, target, sizeof(actual));

        if (MH_CreateHook(target, h.detour, h.original) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            Log(EchoVR::LogLevel::Info,
                "[NEVR.PATCH] hooked name=%s va=0x%llX",
                h.name, static_cast<unsigned long long>(h.va));
            installed++;
        } else {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] hook failed name=%s va=0x%llX "
                "expected=00000000 actual=%02x%02x%02x%02x",
                h.name, static_cast<unsigned long long>(h.va),
                actual[0], actual[1], actual[2], actual[3]);
            if (!failedNames.empty()) failedNames += ", ";
            failedNames += h.name;
            failed++;
        }
    }

    // BUG #13 fix: patch CPrecisionSleep::BusyWait to RET (0xC3)
    // Eliminates tight QPC+SwitchToThread spin loop that starves HT sibling.
    // The WaitableTimer phase in Wait handles the bulk of the sleep;
    // only the final ~250us of busy-wait precision is lost.
    void* busywait = ResolveVA_Safe(g_base, VA_PRECISION_SLEEP_BUSYWAIT);
    if (!busywait) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] hook skipped name=CPrecisionSleep::BusyWait va=0x%llX reason=address_unmapped",
            static_cast<unsigned long long>(VA_PRECISION_SLEEP_BUSYWAIT));
        if (!failedNames.empty()) failedNames += ", ";
        failedNames += "CPrecisionSleep::BusyWait";
        failed++;
    } else {
        // Save original byte before overwriting — N33: restore on Shutdown.
        memcpy(&s_busywait_original_byte, busywait, 1);
        s_busywait_patched = true;
        uint8_t ret_byte = 0xC3;
        ProcessMemcpy(busywait, &ret_byte, 1);
        Log(EchoVR::LogLevel::Info,
            "[NEVR.PATCH] patched name=CPrecisionSleep::BusyWait va=0x%llX (BUG#13 fix, RET patch, saved orig=0x%02X)",
            static_cast<unsigned long long>(VA_PRECISION_SLEEP_BUSYWAIT),
            static_cast<unsigned int>(s_busywait_original_byte));
        installed++;
    }

    // BUG #62 fix: httpport HTTP-listener bind failure must be fatal (server mode).
    // Validate the prologue before hooking — this is going toward production, and a
    // wrong/mismatched target would be a blind hook into the wrong code.
    {
        void* target = ResolveVA_Safe(g_base, VA_HTTP_LISTENER_BRINGUP);
        if (!target) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] hook skipped name=HTTPListenerBringup va=0x%llX reason=address_unmapped",
                static_cast<unsigned long long>(VA_HTTP_LISTENER_BRINGUP));
            if (!failedNames.empty()) failedNames += ", ";
            failedNames += "HTTPListenerBringup";
            failed++;
        } else if (memcmp(target, HTTP_LISTENER_PROLOGUE, sizeof(HTTP_LISTENER_PROLOGUE)) != 0) {
            uint8_t actual[4];
            memcpy(actual, target, 4);
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] hook failed name=HTTPListenerBringup va=0x%llX "
                "expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x reason=prologue_mismatch",
                static_cast<unsigned long long>(VA_HTTP_LISTENER_BRINGUP),
                HTTP_LISTENER_PROLOGUE[0], HTTP_LISTENER_PROLOGUE[1],
                HTTP_LISTENER_PROLOGUE[2], HTTP_LISTENER_PROLOGUE[3],
                actual[0], actual[1], actual[2], actual[3]);
            if (!failedNames.empty()) failedNames += ", ";
            failedNames += "HTTPListenerBringup";
            failed++;
        } else if (MH_CreateHook(target, (void*)&HttpListenerBringupHook,
                                 (void**)&s_origHttpListenerBringup) == MH_OK &&
                   MH_EnableHook(target) == MH_OK) {
            Log(EchoVR::LogLevel::Info,
                "[NEVR.PATCH] hooked name=HTTPListenerBringup va=0x%llX (BUG#62 fix)",
                static_cast<unsigned long long>(VA_HTTP_LISTENER_BRINGUP));
            installed++;
        } else {
            uint8_t actual[4];
            memcpy(actual, target, 4);
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] hook failed name=HTTPListenerBringup va=0x%llX "
                "expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x",
                static_cast<unsigned long long>(VA_HTTP_LISTENER_BRINGUP),
                HTTP_LISTENER_PROLOGUE[0], HTTP_LISTENER_PROLOGUE[1],
                HTTP_LISTENER_PROLOGUE[2], HTTP_LISTENER_PROLOGUE[3],
                actual[0], actual[1], actual[2], actual[3]);
            if (!failedNames.empty()) failedNames += ", ";
            failedNames += "HTTPListenerBringup";
            failed++;
        }
    }

    // Aggregate summary per LOGGING.md Rule 5
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] hooks installed: %d succeeded, %d failed (failed: %s)",
        installed, failed,
        failedNames.empty() ? "none" : failedNames.c_str());

    g_initialized = (installed > 0);
#endif
}

void Wave0::Shutdown() {
    if (!g_initialized) return;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] wave0 shutdown null_deref=%ld",
        s_null_deref_count);
#ifdef _WIN32
    struct { void** orig; uint64_t va; } entries[] = {
        { (void**)&s_origGetTimeMicroseconds, VA_GET_TIME_MICROSECONDS },
        { (void**)&s_origGetTimeMilliseconds, VA_GET_TIME_MILLISECONDS },
        { (void**)&s_origEndMultiplayer, VA_END_MULTIPLAYER },
        { (void**)&s_origPrecisionSleepWait, VA_PRECISION_SLEEP_WAIT },
        { (void**)&s_origWaitForValue, VA_SPINWAIT_WAIT_FOR_VALUE },
        { (void**)&s_origHttpListenerBringup, VA_HTTP_LISTENER_BRINGUP },
    };
    for (auto& e : entries) {
        if (*e.orig != nullptr) {
            MH_DisableHook(ResolveVA(g_base, e.va));
        }
    }
    // N33: restore original byte at BusyWait — the RET patch is a direct
    // ProcessMemcpy, not a MinHook table hook, so it must be undone manually.
    if (s_busywait_patched) {
        void* busywait = ResolveVA_Safe(g_base, VA_PRECISION_SLEEP_BUSYWAIT);
        if (busywait) {
            ProcessMemcpy(busywait, &s_busywait_original_byte, 1);
            Log(EchoVR::LogLevel::Info,
                "[NEVR.PATCH] wave0 shutdown restored BusyWait byte 0x%02X at va=0x%llX",
                static_cast<unsigned int>(s_busywait_original_byte),
                static_cast<unsigned long long>(VA_PRECISION_SLEEP_BUSYWAIT));
        }
        s_busywait_patched = false;
    }
    if (s_cached_timer != NULL) {
        CloseHandle(s_cached_timer);
        s_cached_timer = NULL;
    }
#endif
}

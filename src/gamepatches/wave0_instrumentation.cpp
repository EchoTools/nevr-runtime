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
 * 0b. CTimer_GetMilliSeconds — overflow observation (Low priority)
 *     Millisecond variant overflows after ~10675 days. Observation only.
 *
 * 0c. EndMultiplayer — null deref prevention (BUG #6, High)
 *     Check pointer at arg1+0x2DA0 before double-deref that crashes.
 *
 * 0d. HandleDXError — transient error recovery (BUG #7, High)
 *     Original: all DXGI errors are fatal. Fix: recover from transient
 *     DEVICE_HUNG and WAS_STILL_DRAWING instead of crashing.
 *
 * 0e. CPrecisionSleep::Wait — cached high-res timer (BUG #11, #12, High)
 *     Original: creates/destroys kernel timer each frame (180 kernel
 *     transitions/sec at 90fps) with standard-res timer. Fix: persistent
 *     high-res waitable timer created once at init.
 *
 * 0f. CSpinWait::WaitForValue — corrected backoff (BUG #14, High)
 *     Original: sleep_ms starts at 10 and decrements to 0 (maximum CPU
 *     at peak contention). Fix: start at 0, increment to 10 (proper
 *     increasing backoff). Adds YieldProcessor() for HT friendliness.
 *
 * 0g. CPrecisionSleep::BusyWait — RET patch (BUG #13, High)
 *     Patches first byte to 0xC3 (RET). Eliminates tight QPC+SwitchToThread
 *     spin loop. Only loses ~250us of busy-wait precision per frame.
 * ====================================================================== */

#include "wave0_instrumentation.h"
#include "patch_addresses.h"
#include "common/logging.h"

#ifdef _WIN32
#include <windows.h>
#include <MinHook.h>
#include "process_mem.h"
#endif

/* --------------------------------------------------------------------
 * Address constants (full VAs for ResolveVA)
 * -------------------------------------------------------------------- */

static constexpr uint64_t VA_GET_TIME_MICROSECONDS   = 0x1400D00C0;
static constexpr uint64_t VA_GET_TIME_MILLISECONDS   = 0x1400D0110;
static constexpr uint64_t VA_END_MULTIPLAYER         = 0x140162450;
static constexpr uint64_t VA_HANDLE_DX_ERROR         = 0x140551070;
static constexpr uint64_t VA_PRECISION_SLEEP_WAIT    = 0x1401CE0B0;
static constexpr uint64_t VA_PRECISION_SLEEP_BUSYWAIT = 0x1401CE4C0;
static constexpr uint64_t VA_SPINWAIT_WAIT_FOR_VALUE = 0x141500ED8;

/* CSpinWait::WaitForValue global spin limit offset (from ImageBase) */
static constexpr uintptr_t OFF_SPINWAIT_SPIN_LIMIT   = 0x2034500;

/* GetTimeMicroseconds global data offsets (from ImageBase 0x140000000) */
static constexpr uintptr_t OFF_TIME_OVERRIDE_FLAG  = 0x2099038;  // int64, non-zero = use cached value
static constexpr uintptr_t OFF_TIME_OVERRIDE_VALUE = 0x209CB00;  // uint64, cached microsecond value

/* DXGI transient error codes (as signed HRESULT) */
static constexpr int32_t DXGI_ERROR_DEVICE_HUNG_      = static_cast<int32_t>(0x887A0006);
static constexpr int32_t DXGI_ERROR_WAS_STILL_DRAWING_ = static_cast<int32_t>(0x887A000A);

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

/* Resolve a virtual address from the PC binary to an in-process pointer */
static inline void* ResolveVA(uintptr_t base, uint64_t va) {
    return reinterpret_cast<void*>(base + (va - 0x140000000));
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

static uint64_t __fastcall GetTimeMicrosecondsHook() {
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

    return static_cast<uint64_t>(whole + frac);
}

/* --------------------------------------------------------------------
 * Hook 0b — CTimer_GetMilliSeconds overflow detection
 *
 * Millisecond variant: (perfCount * 1000) / perfFreq.
 * Overflow at INT64_MAX / 1000 = 9,223,372,036,854,775 ticks.
 * At 10MHz: ~10675 days. At 25MHz: ~4270 days. Not urgent.
 * Warn at 90% of 106-day threshold (conservative for high-freq systems).
 * 95 days = 8,208,000,000,000 ms.
 * -------------------------------------------------------------------- */

using GetTimeMilliseconds_t = uint64_t(__fastcall*)();
static GetTimeMilliseconds_t s_origGetTimeMilliseconds = nullptr;
static volatile LONG s_overflow_ms_count = 0;

static uint64_t __fastcall GetTimeMillisecondsHook() {
    uint64_t result = s_origGetTimeMilliseconds();
    static constexpr uint64_t WARN_THRESHOLD_MS = 8208000000000ULL;
    if (result > WARN_THRESHOLD_MS) {
        LONG count = InterlockedIncrement(&s_overflow_ms_count);
        if (count == 1 || (count % 10000) == 0) {
            Log(EchoVR::LogLevel::Warning,
                "[wave0] CTimer_GetMilliSeconds near overflow: %llu ms (count=%ld)",
                (unsigned long long)result, count);
        }
    }
    return result;
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
 * Hook 0d — HandleDXError transient recovery (BUG #7, High)
 *
 * Centralized DXGI error handler with 75+ callers. The original treats
 * every HRESULT failure as fatal (non-returning NRadEngine_LogError).
 *
 * Fix: intercept transient errors that are recoverable:
 *   DXGI_ERROR_DEVICE_HUNG (0x887A0006) — TDR recovery possible
 *   DXGI_ERROR_WAS_STILL_DRAWING (0x887A000A) — GPU busy, retry later
 * Log and return for these instead of calling the fatal original.
 * All other errors pass through to the original fatal handler.
 * -------------------------------------------------------------------- */

using HandleDXError_t = void(__fastcall*)(uint64_t hr, uint64_t context_fmt,
                                          uint64_t detail_str, int64_t extra_info);
static HandleDXError_t s_origHandleDXError = nullptr;
static volatile LONG s_dx_error_count = 0;
static volatile LONG s_dx_transient_count = 0;

static void __fastcall HandleDXErrorHook(uint64_t hr, uint64_t context_fmt,
                                         uint64_t detail_str, int64_t extra_info) {
    int32_t hresult = static_cast<int32_t>(hr);

    if (hresult < 0) {
        const char* detail = detail_str ? reinterpret_cast<const char*>(detail_str) : "(null)";

        // Recover from transient DXGI errors instead of crashing
        if (hresult == DXGI_ERROR_DEVICE_HUNG_ ||
            hresult == DXGI_ERROR_WAS_STILL_DRAWING_) {
            LONG count = InterlockedIncrement(&s_dx_transient_count);
            if (count <= 10 || (count % 1000) == 0) {
                Log(EchoVR::LogLevel::Warning,
                    "[wave0] HandleDXError: transient HRESULT=0x%08X recovered (detail=%s, count=%ld)",
                    static_cast<unsigned int>(hresult), detail, count);
            }
            return;  // Don't call original — it's fatal and non-returning
        }

        LONG count = InterlockedIncrement(&s_dx_error_count);
        Log(EchoVR::LogLevel::Warning,
            "[wave0] HandleDXError: fatal HRESULT=0x%08X detail=%s (count=%ld)",
            static_cast<unsigned int>(hresult), detail, count);
    }

    s_origHandleDXError(hr, context_fmt, detail_str, extra_info);
}

/* --------------------------------------------------------------------
 * Hook 0e — CPrecisionSleep::Wait replacement (BUG #11, #12, High)
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
    (void)unk;
    (void)unk2;

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
 * Hook 0f — CSpinWait::WaitForValue backoff fix (BUG #14, High)
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

static void __fastcall WaitForValueHook(volatile uint32_t* ptr, uint32_t expected, uint32_t mask) {
    // Read configurable spin limit from game's global data
    uint32_t spin_limit = *reinterpret_cast<volatile uint32_t*>(g_base + OFF_SPINWAIT_SPIN_LIMIT);
    if (spin_limit == 0) spin_limit = 4000;

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

#endif  // _WIN32

/* ====================================================================
 * Public API
 * ==================================================================== */

void Wave0::Init(uintptr_t base_addr) {
    g_base = base_addr;

    Log(EchoVR::LogLevel::Info, "[wave0] installing bug fix hooks");

#ifdef _WIN32
    // Cache QPC frequency (constant per process, ~20ns on Windows, ~1-5us on Wine)
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    s_cached_perf_freq = freq.QuadPart;
    Log(EchoVR::LogLevel::Info, "[wave0] QPC frequency: %lld Hz", (long long)s_cached_perf_freq);

    // Create persistent high-res waitable timer (BUG #11, #12 fix)
    // Try high-res first (Windows 10 1803+), fall back to standard
    s_cached_timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (s_cached_timer) {
        Log(EchoVR::LogLevel::Info, "[wave0] created high-resolution waitable timer");
    } else {
        s_cached_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (s_cached_timer) {
            Log(EchoVR::LogLevel::Info, "[wave0] created standard waitable timer (high-res unavailable)");
        } else {
            Log(EchoVR::LogLevel::Warning, "[wave0] failed to create waitable timer, Sleep fallback");
        }
    }

    struct HookEntry {
        uint64_t va;
        void* detour;
        void** original;
        const char* name;
    };

    HookEntry hooks[] = {
        { VA_GET_TIME_MICROSECONDS, (void*)&GetTimeMicrosecondsHook,
          (void**)&s_origGetTimeMicroseconds, "GetTimeMicroseconds (BUG#1 fix)" },
        { VA_GET_TIME_MILLISECONDS, (void*)&GetTimeMillisecondsHook,
          (void**)&s_origGetTimeMilliseconds, "CTimer_GetMilliSeconds" },
        { VA_END_MULTIPLAYER, (void*)&EndMultiplayerHook,
          (void**)&s_origEndMultiplayer, "EndMultiplayer (BUG#6 fix)" },
        { VA_HANDLE_DX_ERROR, (void*)&HandleDXErrorHook,
          (void**)&s_origHandleDXError, "HandleDXError (BUG#7 fix)" },
        { VA_PRECISION_SLEEP_WAIT, (void*)&PrecisionSleepWaitHook,
          (void**)&s_origPrecisionSleepWait, "CPrecisionSleep::Wait (BUG#11/#12 fix)" },
        { VA_SPINWAIT_WAIT_FOR_VALUE, (void*)&WaitForValueHook,
          (void**)&s_origWaitForValue, "CSpinWait::WaitForValue (BUG#14 fix)" },
    };

    int installed = 0;
    for (auto& h : hooks) {
        void* target = ResolveVA(g_base, h.va);
        if (MH_CreateHook(target, h.detour, h.original) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            Log(EchoVR::LogLevel::Info, "[wave0] hooked %s at 0x%llx", h.name,
                (unsigned long long)h.va);
            installed++;
        } else {
            Log(EchoVR::LogLevel::Warning, "[wave0] FAILED to hook %s at 0x%llx", h.name,
                (unsigned long long)h.va);
        }
    }

    // BUG #13 fix: patch CPrecisionSleep::BusyWait to RET (0xC3)
    // Eliminates tight QPC+SwitchToThread spin loop that starves HT sibling.
    // The WaitableTimer phase in Wait handles the bulk of the sleep;
    // only the final ~250us of busy-wait precision is lost.
    void* busywait = ResolveVA(g_base, VA_PRECISION_SLEEP_BUSYWAIT);
    uint8_t ret_byte = 0xC3;
    ProcessMemcpy(busywait, &ret_byte, 1);
    Log(EchoVR::LogLevel::Info, "[wave0] patched CPrecisionSleep::BusyWait -> RET (BUG#13 fix)");
    installed++;

    Log(EchoVR::LogLevel::Info, "[wave0] complete: %d hooks/patches installed",
        installed);
    g_initialized = (installed > 0);
#endif
}

void Wave0::Shutdown() {
    if (!g_initialized) return;
    Log(EchoVR::LogLevel::Info, "[wave0] shutdown — overflow_ms=%ld null_deref=%ld dx_fatal=%ld dx_transient=%ld",
        s_overflow_ms_count, s_null_deref_count, s_dx_error_count, s_dx_transient_count);
#ifdef _WIN32
    struct { void** orig; uint64_t va; } entries[] = {
        { (void**)&s_origGetTimeMicroseconds, VA_GET_TIME_MICROSECONDS },
        { (void**)&s_origGetTimeMilliseconds, VA_GET_TIME_MILLISECONDS },
        { (void**)&s_origEndMultiplayer, VA_END_MULTIPLAYER },
        { (void**)&s_origHandleDXError, VA_HANDLE_DX_ERROR },
        { (void**)&s_origPrecisionSleepWait, VA_PRECISION_SLEEP_WAIT },
        { (void**)&s_origWaitForValue, VA_SPINWAIT_WAIT_FOR_VALUE },
    };
    for (auto& e : entries) {
        if (*e.orig != nullptr) {
            MH_DisableHook(ResolveVA(g_base, e.va));
        }
    }
    if (s_cached_timer != NULL) {
        CloseHandle(s_cached_timer);
        s_cached_timer = NULL;
    }
#endif
}

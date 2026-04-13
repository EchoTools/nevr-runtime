/* SYNTHESIS -- custom tool code, not from binary */

/* ======================================================================
 * wave0_instrumentation — Instrumentation hooks for verified binary bugs
 *
 * Wave 0: observation-only hooks that log when known binary bugs fire.
 * No behavior changes except 0c (EndMultiplayer null deref prevention,
 * where the alternative is a guaranteed crash).
 *
 * Hooks:
 * 0a. GetTimeMicroseconds — overflow detection near 10.68-day QPC limit
 * 0b. CTimer_GetMilliSeconds — overflow detection (conservative threshold)
 * 0c. EndMultiplayer — null deref at +0x2DA0 crash prevention
 * 0d. HandleDXError — HRESULT logging for all DX error paths
 * ====================================================================== */

#include "wave0_instrumentation.h"
#include "patch_addresses.h"
#include "common/logging.h"

#ifdef _WIN32
#include <windows.h>
#include <MinHook.h>
#endif

/* --------------------------------------------------------------------
 * Address constants (full VAs for ResolveVA)
 * -------------------------------------------------------------------- */

static constexpr uint64_t VA_GET_TIME_MICROSECONDS = 0x1400D00C0;
static constexpr uint64_t VA_GET_TIME_MILLISECONDS = 0x1400D0110;
static constexpr uint64_t VA_END_MULTIPLAYER       = 0x140162450;
static constexpr uint64_t VA_HANDLE_DX_ERROR       = 0x140551070;

/* --------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------- */

static uintptr_t g_base = 0;
static bool g_initialized = false;

/* Resolve a virtual address from the PC binary to an in-process pointer */
static inline void* ResolveVA(uintptr_t base, uint64_t va) {
    return reinterpret_cast<void*>(base + (va - 0x140000000));
}

#ifdef _WIN32

/* --------------------------------------------------------------------
 * Hook 0a — GetTimeMicroseconds overflow detection
 *
 * The original computes (perfCount * 1000000) / perfFreq.
 * perfCount at overflow: INT64_MAX / 1000000 = 9,223,372,036,854
 * At 10MHz QPC, that's 922337203 seconds = ~10.68 days.
 * Warn at 90% of overflow threshold (~9.6 days of uptime).
 * result is in microseconds since boot. 9.6 days = 829,440,000,000,000 us.
 * -------------------------------------------------------------------- */

using GetTimeMicroseconds_t = uint64_t(__fastcall*)();
static GetTimeMicroseconds_t s_origGetTimeMicroseconds = nullptr;
static volatile LONG s_overflow_us_count = 0;

static uint64_t __fastcall GetTimeMicrosecondsHook() {
    uint64_t result = s_origGetTimeMicroseconds();
    static constexpr uint64_t WARN_THRESHOLD_US = 829440000000000ULL;
    if (result > WARN_THRESHOLD_US) {
        LONG count = InterlockedIncrement(&s_overflow_us_count);
        if (count == 1 || (count % 10000) == 0) {
            Log(EchoVR::LogLevel::Warning,
                "[wave0] GetTimeMicroseconds near overflow: %llu us (count=%ld)",
                (unsigned long long)result, count);
        }
    }
    return result;
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
 * Hook 0d — HandleDXError logging
 *
 * Centralized DXGI error handler with 75 callers. Logs HRESULT and
 * detail string before passing through to the original fatal handler.
 * -------------------------------------------------------------------- */

using HandleDXError_t = void(__fastcall*)(uint64_t hr, uint64_t context_fmt,
                                          uint64_t detail_str, int64_t extra_info);
static HandleDXError_t s_origHandleDXError = nullptr;
static volatile LONG s_dx_error_count = 0;

static void __fastcall HandleDXErrorHook(uint64_t hr, uint64_t context_fmt,
                                         uint64_t detail_str, int64_t extra_info) {
    int32_t hresult = static_cast<int32_t>(hr);
    if (hresult < 0) {
        LONG count = InterlockedIncrement(&s_dx_error_count);
        const char* detail = detail_str ? reinterpret_cast<const char*>(detail_str) : "(null)";
        Log(EchoVR::LogLevel::Warning,
            "[wave0] HandleDXError: HRESULT=0x%08X detail=%s (count=%ld)",
            static_cast<unsigned int>(hresult), detail, count);
    }
    // Always call original — wave 0 is observation only (except 0c crash prevention)
    s_origHandleDXError(hr, context_fmt, detail_str, extra_info);
}

#endif  // _WIN32

/* ====================================================================
 * Public API
 * ==================================================================== */

void Wave0::Init(uintptr_t base_addr) {
    g_base = base_addr;

    Log(EchoVR::LogLevel::Info, "[wave0] installing instrumentation hooks");

#ifdef _WIN32
    struct HookEntry {
        uint64_t va;
        void* detour;
        void** original;
        const char* name;
    };

    HookEntry hooks[] = {
        { VA_GET_TIME_MICROSECONDS, (void*)&GetTimeMicrosecondsHook,
          (void**)&s_origGetTimeMicroseconds, "GetTimeMicroseconds" },
        { VA_GET_TIME_MILLISECONDS, (void*)&GetTimeMillisecondsHook,
          (void**)&s_origGetTimeMilliseconds, "CTimer_GetMilliSeconds" },
        { VA_END_MULTIPLAYER, (void*)&EndMultiplayerHook,
          (void**)&s_origEndMultiplayer, "EndMultiplayer" },
        { VA_HANDLE_DX_ERROR, (void*)&HandleDXErrorHook,
          (void**)&s_origHandleDXError, "HandleDXError" },
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

    Log(EchoVR::LogLevel::Info, "[wave0] instrumentation complete: %d/%d hooks installed",
        installed, (int)(sizeof(hooks) / sizeof(hooks[0])));
    g_initialized = (installed > 0);
#endif
}

void Wave0::Shutdown() {
    if (!g_initialized) return;
    Log(EchoVR::LogLevel::Info, "[wave0] shutdown — overflow_us=%ld overflow_ms=%ld null_deref=%ld dx_error=%ld",
        s_overflow_us_count, s_overflow_ms_count, s_null_deref_count, s_dx_error_count);
#ifdef _WIN32
    struct { void** orig; uint64_t va; } entries[] = {
        { (void**)&s_origGetTimeMicroseconds, VA_GET_TIME_MICROSECONDS },
        { (void**)&s_origGetTimeMilliseconds, VA_GET_TIME_MILLISECONDS },
        { (void**)&s_origEndMultiplayer, VA_END_MULTIPLAYER },
        { (void**)&s_origHandleDXError, VA_HANDLE_DX_ERROR },
    };
    for (auto& e : entries) {
        if (*e.orig != nullptr) {
            MH_DisableHook(ResolveVA(g_base, e.va));
        }
    }
#endif
}

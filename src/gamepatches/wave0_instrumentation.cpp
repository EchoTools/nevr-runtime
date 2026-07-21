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
#include "patch_addresses.h"
#include "common/logging.h"

#ifdef _WIN32
#include <windows.h>
#include <cstring>
#include <MinHook.h>
#include "process_mem.h"
#include "cli.h"              // g_isServer
#include "crash_recovery.h"   // ForceFatalExit
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
        fprintf(stderr, "[wave0] already initialized, skipping duplicate Init\n"); fflush(stderr);
        return;
    }

    fprintf(stderr, "[wave0] installing bug fix hooks\n"); fflush(stderr);

#ifdef _WIN32
    // Cache QPC frequency (constant per process, ~20ns on Windows, ~1-5us on Wine)
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    s_cached_perf_freq = freq.QuadPart;
    fprintf(stderr, "[wave0] QPC frequency: %lld Hz\n", (long long)s_cached_perf_freq); fflush(stderr);

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
        fprintf(stderr, "[wave0] created high-resolution waitable timer\n"); fflush(stderr);
    } else {
        s_cached_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (s_cached_timer) {
            fprintf(stderr, "[wave0] created standard waitable timer (high-res unavailable)\n"); fflush(stderr);
        } else {
            fprintf(stderr, "[wave0] WARN: failed to create waitable timer, Sleep fallback\n"); fflush(stderr);
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
    for (auto& h : hooks) {
        void* target = ResolveVA_Safe(g_base, h.va);
        if (!target) {
            fprintf(stderr, "[wave0] SKIP %s — address 0x%llx resolved to unmapped memory\n",
                    h.name, (unsigned long long)h.va); fflush(stderr);
            continue;
        }
        if (h.prologue && h.prologue_len > 0 &&
            memcmp(target, h.prologue, h.prologue_len) != 0) {
            fprintf(stderr, "[wave0] SKIP %s — prologue mismatch at 0x%llx "
                    "(binary version drift?)\n",
                    h.name, (unsigned long long)h.va); fflush(stderr);
            continue;
        }
        if (MH_CreateHook(target, h.detour, h.original) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            fprintf(stderr, "[wave0] hooked %s at 0x%llx\n", h.name,
                (unsigned long long)h.va); fflush(stderr);
            installed++;
        } else {
            fprintf(stderr, "[wave0] FAILED to hook %s at 0x%llx\n", h.name,
                (unsigned long long)h.va); fflush(stderr);
        }
    }

    // BUG #13 fix: patch CPrecisionSleep::BusyWait to RET (0xC3)
    // Eliminates tight QPC+SwitchToThread spin loop that starves HT sibling.
    // The WaitableTimer phase in Wait handles the bulk of the sleep;
    // only the final ~250us of busy-wait precision is lost.
    void* busywait = ResolveVA_Safe(g_base, VA_PRECISION_SLEEP_BUSYWAIT);
    if (!busywait) {
        fprintf(stderr, "[wave0] SKIP CPrecisionSleep::BusyWait — address resolved to unmapped memory\n");
        fflush(stderr);
    } else {
        uint8_t ret_byte = 0xC3;
        ProcessMemcpy(busywait, &ret_byte, 1);
        fprintf(stderr, "[wave0] patched CPrecisionSleep::BusyWait -> RET (BUG#13 fix)\n"); fflush(stderr);
        installed++;
    }

    // BUG #62 fix: httpport HTTP-listener bind failure must be fatal (server mode).
    // Validate the prologue before hooking — this is going toward production, and a
    // wrong/mismatched target would be a blind hook into the wrong code.
    {
        void* target = ResolveVA_Safe(g_base, VA_HTTP_LISTENER_BRINGUP);
        if (!target) {
            fprintf(stderr, "[wave0] SKIP HTTP listener bringup — address unmapped\n"); fflush(stderr);
        } else if (memcmp(target, HTTP_LISTENER_PROLOGUE, sizeof(HTTP_LISTENER_PROLOGUE)) != 0) {
            fprintf(stderr, "[wave0] SKIP HTTP listener bringup — prologue mismatch at 0x%llx "
                    "(binary version drift?)\n", (unsigned long long)VA_HTTP_LISTENER_BRINGUP); fflush(stderr);
        } else if (MH_CreateHook(target, (void*)&HttpListenerBringupHook,
                                 (void**)&s_origHttpListenerBringup) == MH_OK &&
                   MH_EnableHook(target) == MH_OK) {
            fprintf(stderr, "[wave0] hooked HTTP listener bringup at 0x%llx (BUG#62 fix)\n",
                    (unsigned long long)VA_HTTP_LISTENER_BRINGUP); fflush(stderr);
            installed++;
        } else {
            fprintf(stderr, "[wave0] FAILED to hook HTTP listener bringup at 0x%llx\n",
                    (unsigned long long)VA_HTTP_LISTENER_BRINGUP); fflush(stderr);
        }
    }

    fprintf(stderr, "[wave0] complete: %d hooks/patches installed\n", installed); fflush(stderr);
    g_initialized = (installed > 0);
#endif
}

void Wave0::Shutdown() {
    if (!g_initialized) return;
    fprintf(stderr, "[wave0] shutdown — null_deref=%ld\n", s_null_deref_count); fflush(stderr);
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
    if (s_cached_timer != NULL) {
        CloseHandle(s_cached_timer);
        s_cached_timer = NULL;
    }
#endif
}

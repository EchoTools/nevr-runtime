/* SYNTHESIS -- custom tool code, not from binary */

/* ======================================================================
 * pnsrad_enabler — Force pnsrad.dll as the social platform
 *
 * EchoVR selects between social platform DLLs ("pnsovr", "pnsdemo", "pnsrad")
 * via flag-driven logic. This module:
 *
 *   1. Overwrites "pnsovr" and "pnsdemo" string data so all code paths load
 *      pnsrad.dll regardless of flags.
 *
 *   2. NOPs the OVR platform branch so PlatformModuleDecisionAndInitialize
 *      takes Path 2 (full networking).
 *
 *   3. Registers an LdrDllNotification callback to patch pnsrad.dll's login
 *      provider check the moment it loads.
 *
 *   4. NOPs LogInSuccessCB's identity comparison so it processes LoginSuccess
 *      even with uninitialized local identity.
 *
 *   5. NOPs LoginIdResponseCB's authenticated state flag check so GameSettings
 *      are processed even when the injected LoginRequest bypasses pnsrad's
 *      state machine.
 * ====================================================================== */

#include "runtime/patch/pnsrad_enabler.h"
#include "core/logging.h"
#include "nevr_common.h"      // N97: the one ValidatePrologue

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#endif

#ifdef _WIN32

/* --------------------------------------------------------------------
 * Patch addresses (RVA from image base)
 * -------------------------------------------------------------------- */

// echovr.exe string data
static constexpr uintptr_t STR_PNSOVR  = 0x16d35c4;
static constexpr uintptr_t STR_PNSDEMO = 0x16d35e8;
static constexpr size_t    STR_SIZE    = 7;  // "pnsrad\0"

// OVR platform branch — 6-byte JNE in PlatformModuleDecisionAndInitialize
static constexpr uintptr_t OVR_BRANCH = 0x1580e5;
static constexpr uint8_t   OVR_JNE_EXPECTED[] = {0x0F, 0x85, 0xC7, 0x00, 0x00, 0x00};

// pnsrad.dll login provider check — JNE at CNSRADUser::vfunction1+0x13
static constexpr uintptr_t PNSRAD_LOGIN_CHECK = 0x85b53;
static constexpr uint8_t   PNSRAD_JNE_EXPECTED[] = {0x75, 0x1f};

// pnsrad.dll LoginIdResponseCB state check — JE at FUN_18008f140+0x76
static constexpr uintptr_t PNSRAD_LOGIN_STATE_CHECK = 0x8f1b6;
static constexpr uint8_t   PNSRAD_STATE_JE_EXPECTED[] = {0x0F, 0x84, 0x78, 0x01, 0x00, 0x00};

// pnsrad.dll LogInSuccessCB session/identity guard — JNE at FUN_18008eea0+0x85
static constexpr uintptr_t PNSRAD_LOGIN_IDENTITY_CHECK = 0x8ef25;
static constexpr uint8_t   PNSRAD_IDENTITY_JNE_EXPECTED[] = {0x0F, 0x85, 0x9C, 0x00, 0x00, 0x00};

/* --------------------------------------------------------------------
 * Memory patching
 * -------------------------------------------------------------------- */

static bool PatchMemory(void* addr, const void* data, size_t len) {
    DWORD oldProtect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(addr, data, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    return true;
}

/* --------------------------------------------------------------------
 * LdrDllNotification — patches pnsrad.dll when it loads
 * -------------------------------------------------------------------- */

typedef struct _LDR_DLL_NOTIFICATION_DATA {
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA;

typedef void (CALLBACK *LDR_DLL_NOTIFICATION_FUNCTION)(ULONG reason,
    const LDR_DLL_NOTIFICATION_DATA* data, void* context);
typedef NTSTATUS (NTAPI *LdrRegisterDllNotification_fn)(ULONG flags,
    LDR_DLL_NOTIFICATION_FUNCTION func, void* context, void** cookie);
typedef NTSTATUS (NTAPI *LdrUnregisterDllNotification_fn)(void* cookie);

static void* s_dllNotifCookie = nullptr;
static bool  s_pnsradPatched  = false;

/* Patch accounting (2026-07-26).
 *
 * These three patches logged success at Debug and prologue-mismatch at Warning,
 * and logged NOTHING AT ALL when PatchMemory itself failed. With min_level=INFO
 * that makes a fully-successful run and a never-fired callback look identical in
 * the log — the N86 failure mode, and the reason "is pnsrad enabled?" could not
 * be answered from a server log.
 *
 * One aggregate at Info, in the shape N17 defined for hook installs. */
static int s_pnsradOk = 0;
static int s_pnsradFail = 0;

/* Apply one NOP patch, counting and reporting every outcome including the
 * previously-silent PatchMemory failure. */
static void PnsradNopPatch(uint8_t* site, const uint8_t* expected, size_t expLen,
                           size_t nopLen, const char* what, unsigned rva) {
    if (!nevr::ValidatePrologue(site, expected, expLen)) {
        Log(EchoVR::LogLevel::Warning,
            "[pnsrad] unexpected bytes at %s +0x%x — NOT patched", what, rva);
        s_pnsradFail++;
        return;
    }
    uint8_t nops[8];
    for (size_t i = 0; i < nopLen && i < sizeof(nops); i++) nops[i] = 0x90;
    if (PatchMemory(site, nops, nopLen)) {
        Log(EchoVR::LogLevel::Debug, "[pnsrad] patched %s at +0x%x", what, rva);
        s_pnsradOk++;
    } else {
        Log(EchoVR::LogLevel::Warning,
            "[pnsrad] PatchMemory FAILED at %s +0x%x — prologue matched but the write "
            "did not land", what, rva);
        s_pnsradFail++;
    }
}

static void CALLBACK OnDllLoaded(ULONG reason, const LDR_DLL_NOTIFICATION_DATA* data, void*) {
    if (reason != 1 || s_pnsradPatched || !data || !data->BaseDllName) return;

    const UNICODE_STRING* name = data->BaseDllName;
    if (name->Length < 10 * sizeof(WCHAR)) return;

    const WCHAR* p = name->Buffer;
    if ((p[0] == L'p' || p[0] == L'P') &&
        (p[1] == L'n' || p[1] == L'N') &&
        (p[2] == L's' || p[2] == L'S') &&
        (p[3] == L'r' || p[3] == L'R') &&
        (p[4] == L'a' || p[4] == L'A') &&
        (p[5] == L'd' || p[5] == L'D') &&
        p[6] == L'.' &&
        (p[7] == L'd' || p[7] == L'D') &&
        (p[8] == L'l' || p[8] == L'L') &&
        (p[9] == L'l' || p[9] == L'L')) {

        s_pnsradPatched = true;
        uintptr_t base = reinterpret_cast<uintptr_t>(data->DllBase);

        PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_CHECK),
                       PNSRAD_JNE_EXPECTED, sizeof(PNSRAD_JNE_EXPECTED), 2,
                       "login check", (unsigned)PNSRAD_LOGIN_CHECK);
        PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_IDENTITY_CHECK),
                       PNSRAD_IDENTITY_JNE_EXPECTED, sizeof(PNSRAD_IDENTITY_JNE_EXPECTED), 6,
                       "identity guard", (unsigned)PNSRAD_LOGIN_IDENTITY_CHECK);
        PnsradNopPatch(reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_STATE_CHECK),
                       PNSRAD_STATE_JE_EXPECTED, sizeof(PNSRAD_STATE_JE_EXPECTED), 6,
                       "state check", (unsigned)PNSRAD_LOGIN_STATE_CHECK);

        Log(EchoVR::LogLevel::Info,
            "[pnsrad] module patches: %d succeeded, %d failed — social layer "
            "(friends/party/login) %s",
            s_pnsradOk, s_pnsradFail,
            (s_pnsradFail == 0 && s_pnsradOk == 3) ? "ENABLED"
            : (s_pnsradOk == 0) ? "NOT PATCHED"
                                : "PARTIALLY PATCHED");
    }
}

#endif // _WIN32

/* ====================================================================
 * Public API
 * ==================================================================== */

void PnsradEnabler::Init(uintptr_t base_addr) {
#ifdef _WIN32
    int patched = 0;

    /* Patch 1: "pnsovr" -> "pnsrad" */
    {
        auto* p = reinterpret_cast<uint8_t*>(base_addr + STR_PNSOVR);
        if (std::memcmp(p, "pnsovr", 6) == 0) {
            if (PatchMemory(p, "pnsrad\0", STR_SIZE)) {
                Log(EchoVR::LogLevel::Info, "[pnsrad] patched \"pnsovr\" -> \"pnsrad\"");
                patched++;
            }
        } else if (std::memcmp(p, "pnsrad", 6) == 0) {
            Log(EchoVR::LogLevel::Debug, "[pnsrad] pnsovr already \"pnsrad\"");
        }
    }

    /* Patch 2: "pnsdemo" -> "pnsrad" */
    {
        auto* p = reinterpret_cast<uint8_t*>(base_addr + STR_PNSDEMO);
        if (std::memcmp(p, "pnsdemo", 7) == 0) {
            if (PatchMemory(p, "pnsrad\0", STR_SIZE)) {
                Log(EchoVR::LogLevel::Info, "[pnsrad] patched \"pnsdemo\" -> \"pnsrad\"");
                patched++;
            }
        } else if (std::memcmp(p, "pnsrad", 6) == 0) {
            Log(EchoVR::LogLevel::Debug, "[pnsrad] pnsdemo already \"pnsrad\"");
        }
    }

    /* Patch 3: OVR platform branch — NOP the 6-byte JNE */
    {
        auto* p = reinterpret_cast<uint8_t*>(base_addr + OVR_BRANCH);
        if (p[0] == 0x90 && p[1] == 0x90) {
            Log(EchoVR::LogLevel::Debug, "[pnsrad] OVR branch already NOPed");
        } else if (nevr::ValidatePrologue(p, OVR_JNE_EXPECTED, sizeof(OVR_JNE_EXPECTED))) {
            uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
            if (PatchMemory(p, nops, sizeof(nops))) {
                Log(EchoVR::LogLevel::Debug, "[pnsrad] patched OVR branch at +0x%x", (unsigned)OVR_BRANCH);
                patched++;
            }
        } else {
            Log(EchoVR::LogLevel::Warning, "[pnsrad] unexpected bytes at OVR branch +0x%x", (unsigned)OVR_BRANCH);
        }
    }

    /* Patch 4 (deferred): Register DLL notification for pnsrad.dll patches */
    {
        HMODULE ntdll = GetModuleHandleA("ntdll");
        auto regFn = reinterpret_cast<LdrRegisterDllNotification_fn>(
            GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        if (regFn) {
            NTSTATUS status = regFn(0, OnDllLoaded, nullptr, &s_dllNotifCookie);
            if (status == 0) {
                Log(EchoVR::LogLevel::Debug, "[pnsrad] registered DLL notification for pnsrad.dll patches");
            } else {
                Log(EchoVR::LogLevel::Warning, "[pnsrad] LdrRegisterDllNotification failed: 0x%lx", (unsigned long)status);
            }
        }
    }

    Log(EchoVR::LogLevel::Info, "[pnsrad] init complete (%d echovr.exe patches)", patched);
#else
    (void)base_addr;
#endif
}

void PnsradEnabler::Shutdown() {
#ifdef _WIN32
    if (s_dllNotifCookie) {
        HMODULE ntdll = GetModuleHandleA("ntdll");
        auto unregFn = reinterpret_cast<LdrUnregisterDllNotification_fn>(
            GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
        if (unregFn) unregFn(s_dllNotifCookie);
        s_dllNotifCookie = nullptr;
    }
#endif
}

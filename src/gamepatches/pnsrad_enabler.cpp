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

#include "pnsrad_enabler.h"
#include "common/logging.h"

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

static bool ValidatePrologue(const uint8_t* site, const uint8_t* expected, size_t len) {
    return std::memcmp(site, expected, len) == 0;
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

        // Patch: login provider check (JNE -> NOP)
        {
            auto* site = reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_CHECK);
            if (ValidatePrologue(site, PNSRAD_JNE_EXPECTED, sizeof(PNSRAD_JNE_EXPECTED))) {
                uint8_t nops[] = {0x90, 0x90};
                if (PatchMemory(site, nops, sizeof(nops))) {
                    Log(EchoVR::LogLevel::Info, "[pnsrad] patched login check at +0x%x", (unsigned)PNSRAD_LOGIN_CHECK);
                }
            } else {
                Log(EchoVR::LogLevel::Warning, "[pnsrad] unexpected bytes at login check +0x%x", (unsigned)PNSRAD_LOGIN_CHECK);
            }
        }

        // Patch: LogInSuccessCB identity guard (JNE -> NOP)
        {
            auto* site = reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_IDENTITY_CHECK);
            if (ValidatePrologue(site, PNSRAD_IDENTITY_JNE_EXPECTED, sizeof(PNSRAD_IDENTITY_JNE_EXPECTED))) {
                uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
                if (PatchMemory(site, nops, sizeof(nops))) {
                    Log(EchoVR::LogLevel::Info, "[pnsrad] patched identity guard at +0x%x", (unsigned)PNSRAD_LOGIN_IDENTITY_CHECK);
                }
            } else {
                Log(EchoVR::LogLevel::Warning, "[pnsrad] unexpected bytes at identity guard +0x%x", (unsigned)PNSRAD_LOGIN_IDENTITY_CHECK);
            }
        }

        // Patch: LoginIdResponseCB state check (JE -> NOP)
        {
            auto* site = reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_STATE_CHECK);
            if (ValidatePrologue(site, PNSRAD_STATE_JE_EXPECTED, sizeof(PNSRAD_STATE_JE_EXPECTED))) {
                uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
                if (PatchMemory(site, nops, sizeof(nops))) {
                    Log(EchoVR::LogLevel::Info, "[pnsrad] patched state check at +0x%x", (unsigned)PNSRAD_LOGIN_STATE_CHECK);
                }
            } else {
                Log(EchoVR::LogLevel::Warning, "[pnsrad] unexpected bytes at state check +0x%x", (unsigned)PNSRAD_LOGIN_STATE_CHECK);
            }
        }
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
        } else if (ValidatePrologue(p, OVR_JNE_EXPECTED, sizeof(OVR_JNE_EXPECTED))) {
            uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
            if (PatchMemory(p, nops, sizeof(nops))) {
                Log(EchoVR::LogLevel::Info, "[pnsrad] patched OVR branch at +0x%x", (unsigned)OVR_BRANCH);
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
                Log(EchoVR::LogLevel::Info, "[pnsrad] registered DLL notification for pnsrad.dll patches");
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

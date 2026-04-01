/* pnsrad_enabler — force pnsrad.dll as the social platform
 *
 * EchoVR selects between social platform DLLs ("pnsovr", "pnsdemo", "pnsrad")
 * via flag-driven logic at ~0x140109xxx. This plugin:
 *
 *   1. Overwrites "pnsovr" and "pnsdemo" string data so all code paths load
 *      pnsrad.dll regardless of flags.
 *
 *   2. NOPs the OVR platform branch (0x1580e5) so
 *      PlatformModuleDecisionAndInitialize takes Path 2 (full networking).
 *
 *   3. Registers an LdrDllNotification callback to patch pnsrad.dll's login
 *      provider check the moment it loads. pnsrad.dll rejects login when
 *      initialized as OVR/DEMO provider — the NOP bypasses that check.
 */

#include "nevr_plugin_interface.h"
#include "nevr_common.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#endif

static void Log(const char* fmt, ...) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[pnsrad_enabler] ");
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
    va_end(args);
    if (n < (int)sizeof(buf) - 1) { buf[n++] = '\n'; buf[n] = '\0'; }
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, buf, n, &written, NULL);
    }
#else
    fputs(buf, stderr);
#endif
}

// echovr.exe string data addresses (RVA from image base)
static constexpr uintptr_t STR_PNSOVR  = 0x16d35c4;
static constexpr uintptr_t STR_PNSDEMO = 0x16d35e8;
static constexpr size_t    STR_SIZE    = 7;  // "pnsrad\0"

// OVR platform branch — 6-byte JNE in PlatformModuleDecisionAndInitialize
static constexpr uintptr_t OVR_BRANCH = 0x1580e5;
static constexpr uint8_t   OVR_JNE_EXPECTED[] = {0x0F, 0x85, 0xC7, 0x00, 0x00, 0x00};

// pnsrad.dll login provider check — JNE at CNSRADUser::vfunction1+0x13
//   180085b49:  call   FUN_1800a00e0
//   180085b51:  test   eax, eax
//   180085b53:  jne    → "Bad log in request: Unsupported provider"
static constexpr uintptr_t PNSRAD_LOGIN_CHECK = 0x85b53;
static constexpr uint8_t   PNSRAD_JNE_EXPECTED[] = {0x75, 0x1f};

#ifdef _WIN32

static bool PatchMemory(void* addr, const void* data, size_t len) {
    DWORD oldProtect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(addr, data, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    return true;
}

// --- LdrRegisterDllNotification ---
// Fires after DllMain but before LoadLibrary returns to the caller.

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
    // reason 1 = LDR_DLL_NOTIFICATION_REASON_LOADED
    if (reason != 1 || s_pnsradPatched || !data || !data->BaseDllName) return;

    // Check if it's pnsrad.dll (case-insensitive wide string compare)
    const UNICODE_STRING* name = data->BaseDllName;
    if (name->Length < 10 * sizeof(WCHAR)) return;  // "pnsrad.dll" = 10 chars

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
        auto* patch_site = reinterpret_cast<uint8_t*>(base + PNSRAD_LOGIN_CHECK);

        if (patch_site[0] == PNSRAD_JNE_EXPECTED[0] && patch_site[1] == PNSRAD_JNE_EXPECTED[1]) {
            uint8_t nops[] = {0x90, 0x90};
            if (PatchMemory(patch_site, nops, sizeof(nops))) {
                Log("patched pnsrad.dll login check at +0x%x (JNE 75 1f -> NOP)", (unsigned)PNSRAD_LOGIN_CHECK);
            } else {
                Log("WARN: VirtualProtect failed for pnsrad.dll login check");
            }
        } else {
            Log("WARN: unexpected bytes at pnsrad.dll +0x%x: %02x %02x (expected 75 1f)",
                (unsigned)PNSRAD_LOGIN_CHECK, patch_site[0], patch_site[1]);
        }
    }
}

#endif // _WIN32

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "pnsrad_enabler";
    info.description = "Forces all social DLL paths to load pnsrad.dll";
    info.version_major = 1;
    info.version_minor = 2;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
#ifdef _WIN32
    uintptr_t base = ctx->base_addr;
    int patched = 0;

    /* Patch 1: "pnsovr" -> "pnsrad" */
    {
        auto* p = reinterpret_cast<uint8_t*>(base + STR_PNSOVR);
        if (std::memcmp(p, "pnsovr", 6) == 0) {
            if (PatchMemory(p, "pnsrad\0", STR_SIZE)) {
                Log("patched \"pnsovr\" -> \"pnsrad\" at +0x%x", (unsigned)STR_PNSOVR);
                patched++;
            }
        } else if (std::memcmp(p, "pnsrad", 6) == 0) {
            Log("pnsovr already \"pnsrad\"");
        } else {
            Log("WARN: unexpected bytes at pnsovr +0x%x", (unsigned)STR_PNSOVR);
        }
    }

    /* Patch 2: "pnsdemo" -> "pnsrad" */
    {
        auto* p = reinterpret_cast<uint8_t*>(base + STR_PNSDEMO);
        if (std::memcmp(p, "pnsdemo", 7) == 0) {
            if (PatchMemory(p, "pnsrad\0", STR_SIZE)) {
                Log("patched \"pnsdemo\" -> \"pnsrad\" at +0x%x", (unsigned)STR_PNSDEMO);
                patched++;
            }
        } else if (std::memcmp(p, "pnsrad", 6) == 0) {
            Log("pnsdemo already \"pnsrad\"");
        } else {
            Log("WARN: unexpected bytes at pnsdemo +0x%x", (unsigned)STR_PNSDEMO);
        }
    }

    /* Patch 3: OVR platform branch — NOP the 6-byte JNE */
    {
        auto* p = reinterpret_cast<uint8_t*>(base + OVR_BRANCH);
        if (p[0] == 0x90 && p[1] == 0x90) {
            Log("OVR branch already NOPed");
        } else if (nevr::ValidatePrologue(p, OVR_JNE_EXPECTED, sizeof(OVR_JNE_EXPECTED))) {
            uint8_t nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
            if (PatchMemory(p, nops, sizeof(nops))) {
                Log("patched OVR branch at +0x%x (JNE -> NOP)", (unsigned)OVR_BRANCH);
                patched++;
            }
        } else {
            Log("WARN: unexpected bytes at OVR branch +0x%x", (unsigned)OVR_BRANCH);
        }
    }

    /* Patch 4 (deferred): Register DLL notification to patch pnsrad.dll login
       check the moment it loads. pnsrad.dll isn't loaded yet at plugin init time. */
    {
        HMODULE ntdll = GetModuleHandleA("ntdll");
        auto regFn = reinterpret_cast<LdrRegisterDllNotification_fn>(
            GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        if (regFn) {
            NTSTATUS status = regFn(0, OnDllLoaded, nullptr, &s_dllNotifCookie);
            if (status == 0) {
                Log("registered DLL notification for pnsrad.dll login patch");
            } else {
                Log("WARN: LdrRegisterDllNotification failed: 0x%lx", (unsigned long)status);
            }
        } else {
            Log("WARN: LdrRegisterDllNotification not found in ntdll");
        }
    }

    Log("init complete (%d echovr.exe patch(es))", patched);
#else
    (void)ctx;
#endif
    return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
#ifdef _WIN32
    if (s_dllNotifCookie) {
        HMODULE ntdll = GetModuleHandleA("ntdll");
        auto unregFn = reinterpret_cast<LdrUnregisterDllNotification_fn>(
            GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
        if (unregFn) unregFn(s_dllNotifCookie);
        s_dllNotifCookie = nullptr;
    }
#endif
    Log("shutting down");
}

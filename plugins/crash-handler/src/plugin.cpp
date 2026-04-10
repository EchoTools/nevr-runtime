/* crash_handler — crash reporter suppression + crash dump logging
 *
 * Intercepts the BugSplat crash reporter chain to prevent Wine errors and
 * keep the process alive after crashes. Installs a VEH that logs a full
 * crash dump (exception info, registers, stack scan with game RVAs) before
 * the process terminates.
 *
 * Hooks:
 *   - CreateProcessA/W: Block BsSndRpt64.exe launch
 *   - ExitProcess: Suppress crash-triggered termination (server mode)
 *   - TerminateProcess: Prevent self-kill after crash reporter block
 *   - VEH: Log crash dump, skip int3 after suppressed ExitProcess
 */

#include "nevr_plugin_interface.h"
#include "nevr_common.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#include <MinHook.h>
#endif

static uintptr_t g_gameBase = 0;
static bool g_isServer = false;

static void PluginLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[crash_handler] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

#ifdef _WIN32

// ============================================================================
// State
// ============================================================================

static bool g_crashReporterSuppressed = false;
static bool g_justSuppressedCrash = false;
static PVOID g_vehHandle = nullptr;

// ============================================================================
// CreateProcessA hook — block BsSndRpt64.exe
// ============================================================================

typedef BOOL(WINAPI* CreateProcessAFunc)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA,
    LPPROCESS_INFORMATION);
static CreateProcessAFunc OrigCreateProcessA = nullptr;

static BOOL WINAPI HookCreateProcessA(LPCSTR lpApp, LPSTR lpCmd,
    LPSECURITY_ATTRIBUTES lpProcAttr, LPSECURITY_ATTRIBUTES lpThreadAttr,
    BOOL bInherit, DWORD dwFlags, LPVOID lpEnv, LPCSTR lpDir,
    LPSTARTUPINFOA lpSI, LPPROCESS_INFORMATION lpPI) {
    if (lpApp && strstr(lpApp, "BsSndRpt")) {
        PluginLog("blocked crash reporter (A): %s", lpApp);
        return FALSE;
    }
    if (lpCmd && strstr(lpCmd, "BsSndRpt")) {
        PluginLog("blocked crash reporter (cmdline A): %s", lpCmd);
        return FALSE;
    }
    return OrigCreateProcessA(lpApp, lpCmd, lpProcAttr, lpThreadAttr,
        bInherit, dwFlags, lpEnv, lpDir, lpSI, lpPI);
}

// ============================================================================
// CreateProcessW hook — block BsSndRpt64.exe (wide)
// ============================================================================

typedef BOOL(WINAPI* CreateProcessWFunc)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
    LPPROCESS_INFORMATION);
static CreateProcessWFunc OrigCreateProcessW = nullptr;

static BOOL WINAPI HookCreateProcessW(LPCWSTR lpApp, LPWSTR lpCmd,
    LPSECURITY_ATTRIBUTES lpProcAttr, LPSECURITY_ATTRIBUTES lpThreadAttr,
    BOOL bInherit, DWORD dwFlags, LPVOID lpEnv, LPCWSTR lpDir,
    LPSTARTUPINFOW lpSI, LPPROCESS_INFORMATION lpPI) {
    if (lpApp && wcsstr(lpApp, L"BsSndRpt")) {
        PluginLog("suppressed crash reporter (W)");
        g_crashReporterSuppressed = true;
        if (lpPI) {
            ZeroMemory(lpPI, sizeof(PROCESS_INFORMATION));
            lpPI->hProcess = (HANDLE)0xDEADBEEF;
            lpPI->hThread = (HANDLE)0xDEADBEEF;
            lpPI->dwProcessId = 0xDEADBEEF;
            lpPI->dwThreadId = 0xDEADBEEF;
        }
        return TRUE;
    }
    if (lpCmd && wcsstr(lpCmd, L"BsSndRpt")) {
        PluginLog("suppressed crash reporter (cmdline W)");
        g_crashReporterSuppressed = true;
        if (lpPI) {
            ZeroMemory(lpPI, sizeof(PROCESS_INFORMATION));
            lpPI->hProcess = (HANDLE)0xDEADBEEF;
            lpPI->hThread = (HANDLE)0xDEADBEEF;
            lpPI->dwProcessId = 0xDEADBEEF;
            lpPI->dwThreadId = 0xDEADBEEF;
        }
        return TRUE;
    }
    return OrigCreateProcessW(lpApp, lpCmd, lpProcAttr, lpThreadAttr,
        bInherit, dwFlags, lpEnv, lpDir, lpSI, lpPI);
}

// ============================================================================
// ExitProcess hook — suppress crash-triggered termination
// ============================================================================

typedef VOID(WINAPI* ExitProcessFunc)(UINT);
static ExitProcessFunc OrigExitProcess = nullptr;

static VOID WINAPI HookExitProcess(UINT uExitCode) {
    if (g_isServer) {
        static volatile LONG exitSuppressCount = 0;
        LONG count = InterlockedIncrement(&exitSuppressCount);
        if (count <= 5) {
            PluginLog("ExitProcess(%u) suppressed in server mode (call #%ld)",
                uExitCode, count);
        }
        g_justSuppressedCrash = true;
        return;
    }

    if (g_crashReporterSuppressed) {
        PluginLog("ExitProcess(%u) suppressed after crash reporter block", uExitCode);
        g_crashReporterSuppressed = false;
        g_justSuppressedCrash = true;
        return;
    }

    PluginLog("ExitProcess(%u) — allowing", uExitCode);
    OrigExitProcess(uExitCode);
}

// ============================================================================
// TerminateProcess hook — prevent self-kill after crash reporter block
// ============================================================================

typedef BOOL(WINAPI* TerminateProcessFunc)(HANDLE, UINT);
static TerminateProcessFunc OrigTerminateProcess = nullptr;

static BOOL WINAPI HookTerminateProcess(HANDLE hProcess, UINT uExitCode) {
    if (hProcess == GetCurrentProcess() || hProcess == (HANDLE)-1) {
        if (g_crashReporterSuppressed) {
            PluginLog("TerminateProcess(self, %u) suppressed after crash reporter block",
                uExitCode);
            g_crashReporterSuppressed = false;
            return TRUE;
        }
        PluginLog("TerminateProcess(self, %u) — allowing", uExitCode);
    }
    return OrigTerminateProcess(hProcess, uExitCode);
}

// ============================================================================
// Crash dump logger
// ============================================================================

static void LogCrashDump(PEXCEPTION_POINTERS ex) {
    PEXCEPTION_RECORD rec = ex->ExceptionRecord;
    PCONTEXT ctx = ex->ContextRecord;
    DWORD64 base = (DWORD64)g_gameBase;

    auto rva = [base](DWORD64 addr) -> INT64 {
        if (addr >= base && addr < base + 0x2000000) return (INT64)(addr - base);
        return -1;
    };

    auto fmtAddr = [base, rva](DWORD64 addr, char* buf, size_t sz) {
        INT64 r = rva(addr);
        if (r >= 0)
            snprintf(buf, sz, "0x%llX (game+0x%llX)",
                (unsigned long long)addr, (unsigned long long)r);
        else
            snprintf(buf, sz, "0x%llX (external)", (unsigned long long)addr);
    };

    const char* excName = "Unknown";
    switch (rec->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:     excName = "ACCESS_VIOLATION"; break;
        case EXCEPTION_BREAKPOINT:           excName = "BREAKPOINT"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:  excName = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_STACK_OVERFLOW:       excName = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:   excName = "INT_DIVIDE_BY_ZERO"; break;
    }

    char ripStr[80];
    fmtAddr(ctx->Rip, ripStr, sizeof(ripStr));

    PluginLog("=== CRASH DUMP ===");
    PluginLog("Exception: %s (0x%08lX)", excName, rec->ExceptionCode);
    PluginLog("RIP: %s", ripStr);

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const char* op = rec->ExceptionInformation[0] == 0 ? "READ"
                       : rec->ExceptionInformation[0] == 1 ? "WRITE"
                                                           : "EXECUTE";
        PluginLog("Access: %s at 0x%llX", op,
            (unsigned long long)rec->ExceptionInformation[1]);
    }

    PluginLog("RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX",
        ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    PluginLog("RSI=%016llX  RDI=%016llX  RBP=%016llX  RSP=%016llX",
        ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
    PluginLog(" R8=%016llX   R9=%016llX  R10=%016llX  R11=%016llX",
        ctx->R8, ctx->R9, ctx->R10, ctx->R11);
    PluginLog("R12=%016llX  R13=%016llX  R14=%016llX  R15=%016llX",
        ctx->R12, ctx->R13, ctx->R14, ctx->R15);

    PluginLog("Stack scan (game code return addresses):");
    DWORD64* sp = (DWORD64*)ctx->Rsp;
    int found = 0;
    for (int i = 0; i < 256 && found < 16; i++) {
        if (IsBadReadPtr(sp + i, 8)) break;
        DWORD64 val = sp[i];
        INT64 r = rva(val);
        if (r >= 0 && r < 0x1800000) {
            PluginLog("  #%d  [RSP+0x%X] game+0x%llX",
                found, i * 8, (unsigned long long)r);
            found++;
        }
    }
    PluginLog("=== END CRASH DUMP ===");
}

// ============================================================================
// VEH — crash dump + int3 skip after suppressed ExitProcess
// ============================================================================

static LONG WINAPI CrashVEH(PEXCEPTION_POINTERS pEx) {
    // Skip int3 after suppressed ExitProcess
    if (pEx->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        g_justSuppressedCrash) {
        PluginLog("int3 after suppressed ExitProcess at RIP=%p — skipping",
            (void*)pEx->ContextRecord->Rip);
        pEx->ContextRecord->Rip += 1;
        g_justSuppressedCrash = false;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Log crash dump for access violations
    if (pEx->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        LogCrashDump(pEx);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// Hook installation helpers
// ============================================================================

static bool InstallKernelHook(HMODULE hK32, const char* name, void* hook, void** orig) {
    void* target = (void*)GetProcAddress(hK32, name);
    if (!target) {
        PluginLog("WARN: %s not found in kernel32", name);
        return false;
    }
    if (MH_CreateHook(target, hook, orig) != MH_OK) {
        PluginLog("WARN: MH_CreateHook failed for %s", name);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        PluginLog("WARN: MH_EnableHook failed for %s", name);
        return false;
    }
    PluginLog("hooked %s", name);
    return true;
}

#endif // _WIN32

// ============================================================================
// Plugin interface
// ============================================================================

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "crash_handler";
    info.description = "Crash reporter suppression + crash dump logging";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
    return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
#ifdef _WIN32
    g_gameBase = ctx->base_addr;
    g_isServer = (ctx->flags & NEVR_HOST_IS_SERVER) != 0;

    if (MH_Initialize() != MH_OK) {
        PluginLog("MH_Initialize failed");
        return -1;
    }

    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    if (!hK32) {
        PluginLog("kernel32.dll not found");
        MH_Uninitialize();
        return -1;
    }

    InstallKernelHook(hK32, "CreateProcessA",
        (void*)HookCreateProcessA, (void**)&OrigCreateProcessA);
    InstallKernelHook(hK32, "CreateProcessW",
        (void*)HookCreateProcessW, (void**)&OrigCreateProcessW);
    InstallKernelHook(hK32, "ExitProcess",
        (void*)HookExitProcess, (void**)&OrigExitProcess);
    InstallKernelHook(hK32, "TerminateProcess",
        (void*)HookTerminateProcess, (void**)&OrigTerminateProcess);

    // VEH with priority 1 (first handler)
    g_vehHandle = AddVectoredExceptionHandler(1, CrashVEH);
    if (g_vehHandle) {
        PluginLog("VEH installed");
    }

    PluginLog("initialized (server=%d)", g_isServer);
#else
    (void)ctx;
#endif
    return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
#ifdef _WIN32
    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
#endif
    PluginLog("shutdown");
}

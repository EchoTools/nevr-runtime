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
#include "plugin_logger.h"
#include "hook_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdarg>

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#include <MinHook.h>
#endif

NEVR_DEFINE_PLUGIN_LOG("[crash_handler]")

static uintptr_t g_gameBase = 0;
static bool g_isServer = false;
static nevr::HookManager g_hooks;

#ifdef _WIN32

// ============================================================================
// State
// ============================================================================

// N67: std::atomic — accessed from CreateProcess hooks (any thread) and
// ExitProcess (any thread) and VEH (faulting thread). Plain bool lacks
// memory ordering guarantees across threads.
static std::atomic<bool> g_crashReporterSuppressed = false;
static std::atomic<bool> g_justSuppressedCrash = false;
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
// Async-signal-safe formatted print for VEH context
// ============================================================================

// Formats a message using vsnprintf to a stack buffer, then writes to stderr
// via WriteFile. No heap, no CRT locks, no fprintf mutex — safe to call from
// Vectored Exception Handler context where the stderr lock may be held by the
// crashing thread or the heap may be corrupted.
static void VehPrintf(const char* fmt, ...) {
    constexpr size_t kBufSize = 1024;
    char buf[kBufSize];

    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, kBufSize - 2, fmt, args);
    va_end(args);
    if (len <= 0) return;

    size_t slen = static_cast<size_t>(len);
    if (slen >= kBufSize - 1) slen = kBufSize - 2;
    buf[slen] = '\n';
    buf[slen + 1] = '\0';

    HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);
    if (!hStderr || hStderr == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hStderr, buf, static_cast<DWORD>(slen + 1), &written, nullptr);
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

    VehPrintf("=== CRASH DUMP ===");
    VehPrintf("Exception: %s (0x%08lX)", excName, rec->ExceptionCode);
    VehPrintf("RIP: %s", ripStr);

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const char* op = rec->ExceptionInformation[0] == 0 ? "READ"
                       : rec->ExceptionInformation[0] == 1 ? "WRITE"
                                                           : "EXECUTE";
        VehPrintf("Access: %s at 0x%llX", op,
            (unsigned long long)rec->ExceptionInformation[1]);
    }

    VehPrintf("RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX",
        ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    VehPrintf("RSI=%016llX  RDI=%016llX  RBP=%016llX  RSP=%016llX",
        ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
    VehPrintf(" R8=%016llX   R9=%016llX  R10=%016llX  R11=%016llX",
        ctx->R8, ctx->R9, ctx->R10, ctx->R11);
    VehPrintf("R12=%016llX  R13=%016llX  R14=%016llX  R15=%016llX",
        ctx->R12, ctx->R13, ctx->R14, ctx->R15);

    VehPrintf("Stack scan (game code return addresses):");
    DWORD64* sp = reinterpret_cast<DWORD64*>(ctx->Rsp);
    int found = 0;
    for (int i = 0; i < 64 && found < 8; i++) {
        MEMORY_BASIC_INFORMATION mbi = {};
        SIZE_T ret = VirtualQuery(static_cast<const void*>(sp + i), &mbi, sizeof(mbi));
        if (ret == 0 ||
            (mbi.State & MEM_COMMIT) == 0 ||
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
            break;
        }
        DWORD64 val = sp[i];
        INT64 r = rva(val);
        if (r >= 0 && r < 0x1800000) {
            VehPrintf("  #%d  [RSP+0x%X] game+0x%llX",
                found, i * 8, (unsigned long long)r);
            found++;
        }
    }
    VehPrintf("=== END CRASH DUMP ===");
}

// ============================================================================
// VEH — crash dump + int3 skip after suppressed ExitProcess
// ============================================================================

static LONG WINAPI CrashVEH(PEXCEPTION_POINTERS pEx) {
    // Skip int3 after suppressed ExitProcess
    if (pEx->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        g_justSuppressedCrash) {
        VehPrintf("int3 after suppressed ExitProcess at RIP=%p — skipping",
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
    MH_STATUS s = g_hooks.CreateAndEnable(target, hook, orig);
    if (s != MH_OK) {
        PluginLog("WARN: hook failed for %s: %d", name, s);
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
    g_hooks.RemoveAll();
#endif
    PluginLog("shutdown");
}

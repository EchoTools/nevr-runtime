#include "crash_recovery.h"

#include <processthreadsapi.h>
#include <psapi.h>
#include <setjmp.h>
#include <windows.h>

#include "cli.h"
#include "common/echovr_functions.h"
#include "common/globals.h"
#include "common/logging.h"
#include "gamepatches_internal.h"
#include "patch_addresses.h"

// Defined in mode_patches.cpp — used by VEH for server crash recovery
extern jmp_buf g_gameLoopJmpBuf;
extern volatile bool g_gameLoopJmpBufValid;

/// <summary>
/// Crash Reporter Suppression (CreateProcessA/W + ExitProcess + TerminateProcess + VEH)
///
/// BugSplat64.dll is a separate third-party DLL imported by echovr.exe that launches
/// BsSndRpt64.exe. The crash reporter launch happens INSIDE BugSplat64.dll, not in game
/// code — there is no single hook point in echovr.exe that controls it. We must intercept
/// at the Windows API level:
///   - CreateProcessA/W: Block BsSndRpt64.exe launch
///   - ExitProcess: Suppress termination after crash reporter block
///   - TerminateProcess: Prevent self-kill after crash reporter block
///   - VEH (BreakpointVEH): Skip int3 padding byte after suppressed ExitProcess return
/// </summary>
typedef BOOL(WINAPI* CreateProcessAFunc)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                         LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
CreateProcessAFunc OriginalCreateProcessA = nullptr;

BOOL WINAPI CreateProcessAHook(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
                               LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
                               LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
                               LPPROCESS_INFORMATION lpProcessInformation) {
  // Block crash reporter executable (BsSndRpt64.exe) to prevent Wine errors
  if (lpApplicationName && strstr(lpApplicationName, "BsSndRpt")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Blocked crash reporter launch (A): %s", lpApplicationName);
    return FALSE;  // Pretend the process failed to start
  }
  if (lpCommandLine && strstr(lpCommandLine, "BsSndRpt")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Blocked crash reporter launch (cmdline A): %s", lpCommandLine);
    return FALSE;
  }

  // Allow all other process launches
  return OriginalCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
                                bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo,
                                lpProcessInformation);
}

/// <summary>
/// Hook for CreateProcessW to disable crash reporter (wide-char version)
/// </summary>
typedef BOOL(WINAPI* CreateProcessWFunc)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                         LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
CreateProcessWFunc OriginalCreateProcessW = nullptr;
static bool g_crashReporterSuppressed = false;
static bool g_justSuppressedCrash = false;

BOOL WINAPI CreateProcessWHook(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                               LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                               BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                               LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                               LPPROCESS_INFORMATION lpProcessInformation) {
  // Block crash reporter executable (BsSndRpt64.exe) to prevent Wine errors
  if (lpApplicationName && wcsstr(lpApplicationName, L"BsSndRpt")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Blocked crash reporter launch (W): %ls", lpApplicationName);
    g_crashReporterSuppressed = true;
    return FALSE;
  }
  if (lpCommandLine && wcsstr(lpCommandLine, L"BsSndRpt")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Blocked crash reporter launch (cmdline W): %ls", lpCommandLine);
    g_crashReporterSuppressed = true;
    return FALSE;
  }

  // Allow all other process launches
  return OriginalCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
                                bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo,
                                lpProcessInformation);
}

/// <summary>
/// Hook for ExitProcess to prevent crash reporter-triggered termination
/// </summary>
typedef VOID(WINAPI* ExitProcessFunc)(UINT);
ExitProcessFunc OriginalExitProcess = nullptr;

VOID WINAPI ExitProcessHook(UINT uExitCode) {
  // In server mode, always suppress ExitProcess — the game's crash reporting
  // chain calls it from multiple places (crash handler, SEH handler, C runtime).
  // We need ALL of them suppressed to keep the server alive.
  if (g_isServer) {
    static volatile LONG exitSuppressCount = 0;
    LONG count = InterlockedIncrement(&exitSuppressCount);
    if (count <= 5) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] ExitProcess(%u) suppressed in server mode (call #%ld)", uExitCode, count);
    }
    g_justSuppressedCrash = true;
    return;
  }

  if (g_crashReporterSuppressed) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] ExitProcess(%u) suppressed after crash reporter block - server continuing", uExitCode);

    void* stack[32];
    USHORT frames = CaptureStackBackTrace(0, 32, stack, NULL);
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Call stack (%u frames):", frames);
    for (USHORT i = 0; i < frames && i < 10; i++) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH]   Frame %u: %p", i, stack[i]);
    }

    g_crashReporterSuppressed = false;
    g_justSuppressedCrash = true;
    return;
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] ExitProcess(%u) called", uExitCode);
  OriginalExitProcess(uExitCode);
}

/// Check whether a memory region is committed and readable without using
/// the deprecated (and unreliable under Wine) IsBadReadPtr.
static bool IsReadableMemory(const void* addr, size_t len) {
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  return true;
}

/// Log a full crash dump: exception info, registers, stack trace with RVAs.
/// All addresses are logged as RVAs relative to the game base so they match
/// revault / Ghidra / IDA directly.
static void LogCrashDump(PEXCEPTION_POINTERS ex) {
  PEXCEPTION_RECORD rec = ex->ExceptionRecord;
  PCONTEXT ctx = ex->ContextRecord;
  DWORD64 base = (DWORD64)EchoVR::g_GameBaseAddress;

  auto rva = [base](DWORD64 addr) -> INT64 {
    if (addr >= base && addr < base + 0x2000000) return (INT64)(addr - base);
    return -1;
  };

  auto fmtAddr = [base, rva](DWORD64 addr, char* buf, size_t sz) {
    INT64 r = rva(addr);
    if (r >= 0)
      snprintf(buf, sz, "0x%llX (game+0x%llX)", (unsigned long long)addr, (unsigned long long)r);
    else
      snprintf(buf, sz, "0x%llX (external)", (unsigned long long)addr);
  };

  // Exception type
  const char* excName = "Unknown";
  switch (rec->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: excName = "ACCESS_VIOLATION"; break;
    case EXCEPTION_BREAKPOINT: excName = "BREAKPOINT"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: excName = "ILLEGAL_INSTRUCTION"; break;
    case EXCEPTION_STACK_OVERFLOW: excName = "STACK_OVERFLOW"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: excName = "INT_DIVIDE_BY_ZERO"; break;
  }

  char ripStr[80];
  fmtAddr(ctx->Rip, ripStr, sizeof(ripStr));

  Log(EchoVR::LogLevel::Error, "=== CRASH DUMP ===");
  Log(EchoVR::LogLevel::Error, "Exception: %s (0x%08lX)", excName, rec->ExceptionCode);
  Log(EchoVR::LogLevel::Error, "RIP: %s", ripStr);

  // Access violation details
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    const char* op = rec->ExceptionInformation[0] == 0 ? "READ"
                   : rec->ExceptionInformation[0] == 1 ? "WRITE"
                                                       : "EXECUTE";
    Log(EchoVR::LogLevel::Error, "Access: %s at 0x%llX", op, (unsigned long long)rec->ExceptionInformation[1]);
  }

  // Register dump
  Log(EchoVR::LogLevel::Error, "RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX",
      ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
  Log(EchoVR::LogLevel::Error, "RSI=%016llX  RDI=%016llX  RBP=%016llX  RSP=%016llX",
      ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
  Log(EchoVR::LogLevel::Error, " R8=%016llX   R9=%016llX  R10=%016llX  R11=%016llX",
      ctx->R8, ctx->R9, ctx->R10, ctx->R11);
  Log(EchoVR::LogLevel::Error, "R12=%016llX  R13=%016llX  R14=%016llX  R15=%016llX",
      ctx->R12, ctx->R13, ctx->R14, ctx->R15);

  // Stack scan — x64 doesn't use frame pointers consistently, so scan RSP
  // for return addresses that point into the game's code range.
  Log(EchoVR::LogLevel::Error, "Stack scan (game code return addresses):");
  DWORD64* sp = (DWORD64*)ctx->Rsp;
  int found = 0;
  for (int i = 0; i < 256 && found < 16; i++) {
    if (!IsReadableMemory(sp + i, 8)) break;
    DWORD64 val = sp[i];
    INT64 r = rva(val);
    if (r >= 0 && r < 0x1800000) {
      Log(EchoVR::LogLevel::Error, "  #%d  [RSP+0x%X] game+0x%llX", found, i * 8, (unsigned long long)r);
      found++;
    }
  }
  // Module listing — identify which DLL owns each address
  Log(EchoVR::LogLevel::Error, "Loaded modules:");
  HANDLE hProcess = GetCurrentProcess();
  HMODULE hMods[256];
  DWORD cbNeeded;
  if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
    DWORD modCount = cbNeeded / sizeof(HMODULE);
    for (DWORD i = 0; i < modCount && i < 256; i++) {
      char modName[MAX_PATH] = {0};
      MODULEINFO mi = {0};
      GetModuleFileNameA(hMods[i], modName, sizeof(modName));
      GetModuleInformation(hProcess, hMods[i], &mi, sizeof(mi));
      // Strip path to just filename
      const char* basename = strrchr(modName, '\\');
      if (!basename) basename = strrchr(modName, '/');
      basename = basename ? basename + 1 : modName;
      DWORD64 modBase = (DWORD64)mi.lpBaseOfDll;
      DWORD64 modEnd = modBase + mi.SizeOfImage;
      // Flag the module that contains the crash RIP
      const char* flag = (ctx->Rip >= modBase && ctx->Rip < modEnd) ? " <-- CRASH" : "";
      Log(EchoVR::LogLevel::Error, "  %016llX-%016llX  %s%s",
          (unsigned long long)modBase, (unsigned long long)modEnd, basename, flag);
    }
  }

  Log(EchoVR::LogLevel::Error, "=== END CRASH DUMP ===");
}

/// <summary>
/// Vectored Exception Handler to skip the int3 instruction that follows the ExitProcess call site
/// in the game's fatal error handler. After our ExitProcessHook returns (suppressing the exit),
/// the CPU executes the int3 padding byte at the return address, which would kill the process.
/// We advance RIP by 1 to skip it and continue execution.
/// </summary>
/// Counter for access violation recoveries
static volatile LONG g_avRecoveryCount = 0;

LONG WINAPI BreakpointVEH(PEXCEPTION_POINTERS pExceptionInfo) {
  if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && g_justSuppressedCrash) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] int3 after suppressed ExitProcess at RIP=%p — skipping, server continuing",
        (void*)pExceptionInfo->ContextRecord->Rip);
    pExceptionInfo->ContextRecord->Rip += 1;
    g_justSuppressedCrash = false;
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  // In server mode, catch null-pointer access violations and recover via longjmp
  if (g_isServer && pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    DWORD64 target = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];

    if (target < 0x10000 && g_gameLoopJmpBufValid) {
      LONG count = InterlockedIncrement(&g_avRecoveryCount);
      if (count <= 3) LogCrashDump(pExceptionInfo);

      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] Null-ptr AV #%ld — longjmp to server hold", count);
      g_gameLoopJmpBufValid = false;
      longjmp(g_gameLoopJmpBuf, (int)count);
    }
  }

  // Log any unhandled fatal exception before passing to the default handler
  DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ACCESS_VIOLATION ||
      code == EXCEPTION_ILLEGAL_INSTRUCTION ||
      code == EXCEPTION_STACK_OVERFLOW ||
      code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
      code == STATUS_STACK_BUFFER_OVERRUN) {
    static volatile LONG g_crashLogCount = 0;
    if (InterlockedIncrement(&g_crashLogCount) <= 3) {
      LogCrashDump(pExceptionInfo);
    }
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

typedef BOOL(WINAPI* TerminateProcessFunc)(HANDLE, UINT);
TerminateProcessFunc OriginalTerminateProcess = nullptr;

BOOL WINAPI TerminateProcessHook(HANDLE hProcess, UINT uExitCode) {
  HANDLE currentProcess = GetCurrentProcess();
  if (hProcess == currentProcess || hProcess == (HANDLE)-1) {
    if (g_crashReporterSuppressed) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] TerminateProcess(self, %u) suppressed after crash reporter block - server continuing",
          uExitCode);
      g_crashReporterSuppressed = false;
      return TRUE;
    }
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] TerminateProcess(self, %u) called - allowing", uExitCode);
  }

  return OriginalTerminateProcess(hProcess, uExitCode);
}

void InstallCrashRecoveryHooks() {
  // Hook CreateProcessA/W to disable crash reporter in Wine/headless mode
  HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
  if (hKernel32 != NULL) {
    OriginalCreateProcessA = (CreateProcessAFunc)GetProcAddress(hKernel32, "CreateProcessA");
    if (OriginalCreateProcessA != NULL) {
      PatchDetour(&OriginalCreateProcessA, reinterpret_cast<PVOID>(CreateProcessAHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateProcessA hook installed (crash reporter disabled)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateProcessA");
    }

    OriginalCreateProcessW = (CreateProcessWFunc)GetProcAddress(hKernel32, "CreateProcessW");
    if (OriginalCreateProcessW != NULL) {
      PatchDetour(&OriginalCreateProcessW, reinterpret_cast<PVOID>(CreateProcessWHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateProcessW hook installed (crash reporter disabled)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateProcessW");
    }

    OriginalExitProcess = (ExitProcessFunc)GetProcAddress(hKernel32, "ExitProcess");
    if (OriginalExitProcess != NULL) {
      PatchDetour(&OriginalExitProcess, reinterpret_cast<PVOID>(ExitProcessHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] ExitProcess hook installed (prevents crash reporter termination)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find ExitProcess");
    }

    OriginalTerminateProcess = (TerminateProcessFunc)GetProcAddress(hKernel32, "TerminateProcess");
    if (OriginalTerminateProcess != NULL) {
      PatchDetour(&OriginalTerminateProcess, reinterpret_cast<PVOID>(TerminateProcessHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] TerminateProcess hook installed (prevents self-termination)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find TerminateProcess");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load kernel32.dll for crash reporter hooks");
  }
}

void InstallVEH() {
  // Install VEH to handle int3 that fires after our ExitProcess suppression returns
  AddVectoredExceptionHandler(1, BreakpointVEH);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Breakpoint VEH installed (handles int3 after ExitProcess suppression)");
}

void InstallConsoleCtrlHandler() {
  // Install console ctrl handler so CTRL+C actually terminates the process.
  // The game registers its own handler that logs "Console close signal received" but doesn't exit.
  // Handlers are called LIFO, so ours runs first and terminates before the game's handler can swallow the signal.
  SetConsoleCtrlHandler(
      [](DWORD dwCtrlType) -> BOOL {
        if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Console signal %lu received — exiting", dwCtrlType);
          if (OriginalExitProcess)
            OriginalExitProcess(0);
          else
            ExitProcess(0);
          return TRUE;  // unreachable, but satisfies the signature
        }
        return FALSE;
      },
      TRUE);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Console ctrl handler installed (CTRL+C will terminate)");
}

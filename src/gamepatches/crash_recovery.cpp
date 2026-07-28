#include "crash_recovery.h"

#include <processthreadsapi.h>
#include <psapi.h>
#include <setjmp.h>
#include <signal.h>
#include <windows.h>

#include <atomic>
#include <cstdarg>

#include <unistd.h>

#include "cli.h"
#include "common/echovr_functions.h"
#include "common/globals.h"
#include "common/logging.h"
#include "gamepatches_internal.h"
#include "patch_addresses.h"
#include "wave0_instrumentation.h"

// Defined in mode_patches.cpp — used by VEH for server crash recovery
extern jmp_buf g_gameLoopJmpBuf;
extern volatile bool g_gameLoopJmpBufValid;

// N62: set for the duration of a signal-context shutdown. `volatile sig_atomic_t`
// is the only type the C standard permits a handler to touch. Read by the
// shutdown path to choose an async-signal-safe reporting transport.
static volatile sig_atomic_t g_inSignalContext = 0;


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

// N67 (re-opened 2026-07-26): these two flags are written from CreateProcessWHook,
// ExitProcessHook and TerminateProcessHook — any thread — and read/written from
// BreakpointVEH on the faulting thread. Plain `bool` gives no ordering guarantee and
// permits the compiler to sink or reorder the stores, so the VEH can observe a stale
// value and either skip an int3 it should have taken or take one it should not.
// The earlier fix converted the copies in plugins/crash-handler/, which is not built
// (plugins/CMakeLists.txt:12) — this is the path that ships.
static std::atomic<bool> g_crashReporterSuppressed{false};
static std::atomic<bool> g_justSuppressedCrash{false};

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

// Set by ForceFatalExit when an intentional exit is underway. While true, the
// server-mode ExitProcess suppression is lifted so the real termination — and any
// ExitProcess the OS/CRT re-enters during it — can actually complete. Without this
// a server-mode exit deadlocks: the termination's own ExitProcess calls get
// re-suppressed and the process spins forever instead of dying.
volatile bool g_forceExitInProgress = false;

VOID WINAPI ExitProcessHook(UINT uExitCode) {
  // Intentional fail-loud / graceful exit underway — never suppress; let it die.
  if (g_forceExitInProgress) {
    if (OriginalExitProcess != nullptr) OriginalExitProcess(uExitCode);
    return;  // if original is null, returns to ForceFatalExit's TerminateProcess fallback
  }
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
  (void)len;
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  return true;
}

// ============================================================================
// N70 — crash-safe output primitive
// ============================================================================
//
// A crash handler may call only what is guaranteed to work when the process has
// already failed: the heap may be corrupt, ANY lock may be held by the faulting
// thread, and the loader lock may be owned.
//
// Log() satisfies none of those. It traverses EchoVR::WriteLog -> the game's CLog
// -> the hooked CLog::PrintfImpl -> EmitLine -> std::lock_guard(g_file_mutex)
// (builtin_log_filter.cpp:755) -> fwrite/fflush. A fault raised while the faulting
// thread already held g_file_mutex would deadlock the crash handler on its own log
// mutex — the dump becomes the hang.
//
// This is NOT the "no printf, use the structured logger" case the CPP addendum
// forbids. The addendum's own Flight Recorder section prescribes exactly this
// mechanism for the crash path ("a dump handler writes the buffer to disk via
// WriteFile/MapViewOfFile") and requires the logger be "safe to call from DllMain
// (no heap, no synchronization primitives)" — which Log() is not and this is.
// Output stays structured key=value; only the transport changes.
//
// vsnprintf into a fixed stack buffer is the standard crash-handler primitive: for
// the %s/%d/%u/%llX conversions used here MinGW's implementation performs no
// allocation. No float conversions are used (those may allocate on some libcs).

static void VehWrite(const char* buf, size_t len) {
  HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
  if (hErr == nullptr || hErr == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(hErr, buf, static_cast<DWORD>(len), &written, nullptr);
}

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
  VehWrite(buf, slen + 1);
}

// ============================================================================
// N70 — module table cached at init, never enumerated from the handler
// ============================================================================
//
// EnumProcessModules / GetModuleFileNameA / GetModuleInformation all serialise on
// the loader lock. A fault raised while that lock is held — i.e. anywhere inside
// LoadModule / LoadPlugins / the game's own CSysDLL_Load — would deadlock a handler
// that enumerates. Snapshot once at init; the handler reads the snapshot.

struct CachedModule {
  DWORD64 base;
  DWORD64 end;
  char name[64];
};

static CachedModule g_moduleCache[192];
static constexpr int kModuleCacheCapacity =
    static_cast<int>(sizeof(g_moduleCache) / sizeof(g_moduleCache[0]));
static volatile LONG g_moduleCacheCount = 0;

static void CacheModuleTable() {
  HANDLE hProcess = GetCurrentProcess();
  HMODULE hMods[192];
  DWORD cbNeeded = 0;
  if (!EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) return;

  const DWORD modCount = cbNeeded / sizeof(HMODULE);
  int cached = 0;
  for (DWORD i = 0; i < modCount && cached < kModuleCacheCapacity; i++) {
    char modName[MAX_PATH] = {0};
    MODULEINFO mi = {};
    if (!GetModuleFileNameA(hMods[i], modName, sizeof(modName))) continue;
    if (!GetModuleInformation(hProcess, hMods[i], &mi, sizeof(mi))) continue;

    const char* basename = strrchr(modName, '\\');
    if (!basename) basename = strrchr(modName, '/');
    basename = basename ? basename + 1 : modName;

    g_moduleCache[cached].base = reinterpret_cast<DWORD64>(mi.lpBaseOfDll);
    g_moduleCache[cached].end = g_moduleCache[cached].base + mi.SizeOfImage;

    // Explicit bounded copy — strncpy here trips -Wstringop-truncation because the
    // compiler can see basename may exceed the field. Truncation is intended (the
    // field holds a basename for a crash line), so state the length arithmetic
    // rather than relying on strncpy's padding semantics.
    constexpr size_t kNameCap = sizeof(g_moduleCache[0].name) - 1;
    size_t nameLen = strlen(basename);
    if (nameLen > kNameCap) nameLen = kNameCap;
    memcpy(g_moduleCache[cached].name, basename, nameLen);
    g_moduleCache[cached].name[nameLen] = '\0';
    cached++;
  }
  InterlockedExchange(&g_moduleCacheCount, cached);
}

// ============================================================================
// N69 — stack reserve so the overflow handler has room to run
// ============================================================================
//
// A stack overflow is the one fault where the reporting mechanism is itself the
// resource that ran out. Windows delivers the exception on the faulting thread's
// stack, which by definition has just hit its guard page. SetThreadStackGuarantee
// reserves extra pages BELOW the guard page so a handler can still execute.
//
// The reserve must exceed the handler's worst-case frame. WriteCrashDump's frame is
// dominated by VehPrintf's 1024-byte buffer plus a 96-byte address buffer and the
// VirtualQuery MEMORY_BASIC_INFORMATION; 64 KiB is ~16x that headroom and is the
// smallest value Windows will round up to a useful multiple on x64.
//
// Per-thread by design: a thread that never calls this has no reserve. Called from
// InstallVEH (main thread) and once per thread from the per-frame hook, so every
// thread that runs NEVR code is covered. Threads created by the game that never
// enter our hooks remain uncovered — recorded as a known limit, not a claim.
static constexpr ULONG kCrashHandlerStackReserve = 64 * 1024;

void EnsureStackReserve() {
  static thread_local bool s_reserved = false;
  if (s_reserved) return;
  s_reserved = true;
  ULONG bytes = kCrashHandlerStackReserve;
  SetThreadStackGuarantee(&bytes);
}


// ============================================================================
// N71 — known session-flags null-deref sites
// ============================================================================
//
// These functions load *(this + 0x2DA0) and immediately dereference it with no
// null check. Verified by disassembly, e.g. DispatchEvent @0x140c540a0:
//   0x140c541a0  MOV RAX, qword ptr [RDI + 0x2da0]   ; game_flags ptr
//   0x140c541a7  MOV RCX, qword ptr [RAX]            ; <-- AV when RAX == 0
//
// DELIBERATELY NOT HOOKED. The original plan was a MinHook null-guard on each of
// ~25-30 functions. Rejected on evidence:
//
//   1. BreakpointVEH already catches this ENTIRE class — any AV with a target
//      below 0x10000 in server mode is dumped and recovered via longjmp. That
//      covers these 19 sites AND every site nobody enumerated, which a hand-built
//      list can never do.
//   2. Every hook is blast radius. Two hooks added for a hazard that was never
//      re-derived (N83) silently severed the ServerDB message path for months.
//      DispatchEvent is per-event; 10 hooks there is per-event overhead for a
//      fault no one has observed.
//   3. Only ONE site in this family has ever been seen to fault (EndMultiplayer,
//      BUG#6), and it is already guarded in Wave0.
//
// What was actually missing is attribution: when the VEH fires, an operator got a
// bare RVA and had to resolve it by hand. This table closes that gap — it costs
// one linear scan on a path that is already crashing, and adds no runtime hook.
struct KnownNullDerefSite {
  DWORD64 rva;
  const char* name;
};

static const KnownNullDerefSite kN71Sites[] = {
    // DispatchEvent family — bit-test on game_flags
    {0xC540A0, "DispatchEvent[0]"},  {0xC54470, "DispatchEvent[1]"},
    {0xC54840, "DispatchEvent[2]"},  {0xC553B0, "DispatchEvent[3]"},
    {0xC55780, "DispatchEvent[4]"},  {0xC55B50, "DispatchEvent[5]"},
    {0xC55F20, "DispatchEvent[6]"},  {0xC562F0, "DispatchEvent[7]"},
    {0xC566C0, "DispatchEvent[8]"},  {0xC56A90, "DispatchEvent[9]"},
    // Combat-mode queries
    {0xD09D60, "IsCompactPoolHandleValid_B"},
    {0xD098C0, "IsPunchableInCombatMode"},
    {0xD09E80, "GetPlayerBlockingState"},
    {0xD09CD0, "LookupPlayerWeaponHandle"},
    {0xD09DB0, "IsPlayerInUnassignedWeaponState"},
    {0xD09EB0, "IsPlayerInPunchState"},
    // Loadout broadcast
    {0x12C2D0, "LoadoutBroadcast[0]"}, {0x130B00, "LoadoutBroadcast[1]"},
    {0x130E00, "LoadoutBroadcast[2]"}, {0x1A9B20, "LoadoutBroadcast[3]"},
    {0x1A9D60, "LoadoutBroadcast[4]"},
    // Miscellaneous
    {0x14E540, "~CR15NetLobby"},      {0x1B1910, "FindSpawnPoint"},
    {0x15F530, "OnMsgCurrentLoadoutRequest"},
    {0x1A79D0, "OnMsgSaveLoadoutRequest"},
    {0x1C98C0, "GetUserName"},        {0x113A90, "GetNetGameFromContext"},
    {0x170770, "GetHeadsetTypeName"}, {0x170730, "GetHeadsetTypeName_B"},
    {0x170750, "GetHeadsetTypeName_C"},
};

/// Returns the enclosing known site for a game RVA, or nullptr. Sites are
/// function entries; a fault lands a little past one, so match the nearest entry
/// at or below the RVA within a plausible function span.
static const char* LookupKnownNullDerefSite(INT64 rva) {
  if (rva < 0) return nullptr;
  const DWORD64 r = static_cast<DWORD64>(rva);
  const char* best = nullptr;
  DWORD64 bestBase = 0;
  for (const auto& site : kN71Sites) {
    if (r >= site.rva && r - site.rva < 0x800 && site.rva >= bestBase) {
      best = site.name;
      bestBase = site.rva;
    }
  }
  return best;
}

/// Write a full crash dump: exception info, registers, stack trace with RVAs.
/// All addresses are emitted as RVAs relative to the game base so they match
/// revault / Ghidra / IDA directly.
///
/// N70: crash-safe by construction. Every call below is a raw syscall
/// (WriteFile / GetStdHandle / VirtualQuery) or a stack-only operation. No Log(),
/// no heap, no mutex, no loader-lock call. Structured key=value output is
/// preserved; only the transport is raw.
static void WriteCrashDump(PEXCEPTION_POINTERS ex) {
  PEXCEPTION_RECORD rec = ex->ExceptionRecord;
  PCONTEXT ctx = ex->ContextRecord;
  const DWORD64 base = reinterpret_cast<DWORD64>(EchoVR::g_GameBaseAddress);

  auto rva = [base](DWORD64 addr) -> INT64 {
    if (addr >= base && addr < base + 0x2000000) return static_cast<INT64>(addr - base);
    return -1;
  };

  const char* excName = "Unknown";
  switch (rec->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: excName = "ACCESS_VIOLATION"; break;
    case EXCEPTION_BREAKPOINT: excName = "BREAKPOINT"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: excName = "ILLEGAL_INSTRUCTION"; break;
    case EXCEPTION_STACK_OVERFLOW: excName = "STACK_OVERFLOW"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: excName = "INT_DIVIDE_BY_ZERO"; break;
    // 0x20474343 == 'GCC ' — the GNU C++ throw magic used by MinGW's __cxa_throw.
    // The game is MSVC-built and cannot raise this; it can only come from a NEVR
    // DLL. An unhandled one reaches the game's top-level filter and kills the
    // server (CPP addendum: never throw across a DLL boundary).
    case 0x20474343: excName = "CXX_THROW_FROM_NEVR_DLL"; break;
    default: break;
  }

  const INT64 ripRva = rva(ctx->Rip);
  VehPrintf("[NEVR.CRASH] === CRASH DUMP ===");
  if (const char* site = LookupKnownNullDerefSite(ripRva)) {
    VehPrintf("[NEVR.CRASH] known_site=%s class=session_flags_null_deref ledger=N71 "
              "note=*(this+0x2DA0) dereferenced without a null check",
              site);
  }
  VehPrintf("[NEVR.CRASH] exception name=%s code=0x%08lX rip=0x%llX rip_rva=%s0x%llX tid=%lu",
            excName, rec->ExceptionCode, static_cast<unsigned long long>(ctx->Rip),
            ripRva >= 0 ? "game+" : "external:",
            static_cast<unsigned long long>(ripRva >= 0 ? ripRva : ctx->Rip),
            GetCurrentThreadId());

  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    const char* op = rec->ExceptionInformation[0] == 0   ? "READ"
                     : rec->ExceptionInformation[0] == 1 ? "WRITE"
                                                         : "EXECUTE";
    VehPrintf("[NEVR.CRASH] access op=%s addr=0x%llX", op,
              static_cast<unsigned long long>(rec->ExceptionInformation[1]));
  }

  // N69: on stack overflow the remaining stack is whatever SetThreadStackGuarantee
  // reserved. Report the fault and the reserve, then stop — the stack scan below
  // would consume frames we may not have.
  if (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
    ULONG reserve = 0;
    SetThreadStackGuarantee(&reserve);  // query form: returns the current guarantee
    VehPrintf("[NEVR.CRASH] stack_overflow rsp=0x%llX reserve_bytes=%lu",
              static_cast<unsigned long long>(ctx->Rsp), reserve);
    VehPrintf("[NEVR.CRASH] === END CRASH DUMP (stack overflow: scan suppressed) ===");
    return;
  }

  VehPrintf("[NEVR.CRASH] regs rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX",
            ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
  VehPrintf("[NEVR.CRASH] regs rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX",
            ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
  VehPrintf("[NEVR.CRASH] regs r8=%016llX r9=%016llX r10=%016llX r11=%016llX",
            ctx->R8, ctx->R9, ctx->R10, ctx->R11);
  VehPrintf("[NEVR.CRASH] regs r12=%016llX r13=%016llX r14=%016llX r15=%016llX",
            ctx->R12, ctx->R13, ctx->R14, ctx->R15);

  // Stack scan — x64 doesn't use frame pointers consistently, so scan RSP
  // for return addresses that point into the game's code range.
  DWORD64* sp = reinterpret_cast<DWORD64*>(ctx->Rsp);
  int found = 0;
  for (int i = 0; i < 512 && found < 24; i++) {
    if (!IsReadableMemory(sp + i, 8)) break;
    const DWORD64 v = sp[i];
    const INT64 r = rva(v);
    if (r >= 0 && r < 0x1800000) {
      VehPrintf("[NEVR.CRASH] frame #%d rsp_off=0x%X mod=echovr.exe rva=0x%llX", found, i * 8,
                static_cast<unsigned long long>(r));
      found++;
      continue;
    }
    // Attribute to a NEVR module — this is what names the DLL that threw.
    const LONG mc = g_moduleCacheCount;
    for (LONG m = 0; m < mc; m++) {
      if (v >= g_moduleCache[m].base && v < g_moduleCache[m].end) {
        VehPrintf("[NEVR.CRASH] frame #%d rsp_off=0x%X mod=%s rva=0x%llX", found, i * 8,
                  g_moduleCache[m].name,
                  static_cast<unsigned long long>(v - g_moduleCache[m].base));
        found++;
        break;
      }
    }
  }
  VehPrintf("[NEVR.CRASH] stack_scan frames=%d", found);

  // Module listing from the init-time snapshot — never enumerated here (loader lock).
  const LONG modCount = g_moduleCacheCount;
  for (LONG i = 0; i < modCount; i++) {
    const bool isCrashModule = ctx->Rip >= g_moduleCache[i].base && ctx->Rip < g_moduleCache[i].end;
    if (!isCrashModule) continue;
    VehPrintf("[NEVR.CRASH] crash_module name=%s base=0x%llX end=0x%llX", g_moduleCache[i].name,
              static_cast<unsigned long long>(g_moduleCache[i].base),
              static_cast<unsigned long long>(g_moduleCache[i].end));
  }
  VehPrintf("[NEVR.CRASH] modules cached=%ld (snapshot taken at init)", modCount);
  VehPrintf("[NEVR.CRASH] === END CRASH DUMP ===");
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
  // N70: every path below uses VehPrintf (raw WriteFile), never Log(). Log()
  // reaches std::lock_guard(g_file_mutex) in the log filter, so a fault raised
  // while the faulting thread held that mutex would deadlock the handler.
  if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
      g_justSuppressedCrash.load(std::memory_order_acquire)) {
    VehPrintf("[NEVR.CRASH] int3_skipped rip=0x%llX reason=after_suppressed_exitprocess",
              static_cast<unsigned long long>(pExceptionInfo->ContextRecord->Rip));
    pExceptionInfo->ContextRecord->Rip += 1;
    g_justSuppressedCrash.store(false, std::memory_order_release);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  // In server mode, catch null-pointer access violations and recover via longjmp.
  //
  // KNOWN RESIDUAL (N70 item 3): longjmp out of a vectored exception handler
  // abandons the x64 unwind without honouring unwind data. It is retained because
  // it is the mechanism that keeps a dedicated server alive through a null-deref in
  // the entity paths, and removing it is a behavioural change to crash recovery, not
  // an observability fix. Tracked in N70; the acute hazards (log-mutex deadlock,
  // loader-lock enumeration) are fixed here.
  if (g_isServer && pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    DWORD64 target = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];

    if (target < 0x10000 && g_gameLoopJmpBufValid) {
      LONG count = InterlockedIncrement(&g_avRecoveryCount);
      if (count <= 3) WriteCrashDump(pExceptionInfo);

      VehPrintf("[NEVR.CRASH] null_ptr_av count=%ld action=longjmp_to_server_hold", count);
      g_gameLoopJmpBufValid = false;
      longjmp(g_gameLoopJmpBuf, static_cast<int>(count));
    }
  }

  // Report any unhandled fatal exception before passing to the default handler.
  DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ACCESS_VIOLATION ||
      code == EXCEPTION_ILLEGAL_INSTRUCTION ||
      code == EXCEPTION_STACK_OVERFLOW ||
      code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
      code == STATUS_STACK_BUFFER_OVERRUN ||
      code == 0x20474343) {  // C++ throw from a NEVR DLL — see WriteCrashDump
    static volatile LONG g_crashLogCount = 0;
    if (InterlockedIncrement(&g_crashLogCount) <= 3) {
      WriteCrashDump(pExceptionInfo);
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
      PatchDetour(&OriginalCreateProcessA, reinterpret_cast<PVOID>(CreateProcessAHook), "CreateProcessA");
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateProcessA hook installed (crash reporter disabled)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateProcessA");
    }

    OriginalCreateProcessW = (CreateProcessWFunc)GetProcAddress(hKernel32, "CreateProcessW");
    if (OriginalCreateProcessW != NULL) {
      PatchDetour(&OriginalCreateProcessW, reinterpret_cast<PVOID>(CreateProcessWHook), "CreateProcessW");
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateProcessW hook installed (crash reporter disabled)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateProcessW");
    }

    OriginalExitProcess = (ExitProcessFunc)GetProcAddress(hKernel32, "ExitProcess");
    if (OriginalExitProcess != NULL) {
      PatchDetour(&OriginalExitProcess, reinterpret_cast<PVOID>(ExitProcessHook), "ExitProcess");
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] ExitProcess hook installed (prevents crash reporter termination)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find ExitProcess");
    }

    OriginalTerminateProcess = (TerminateProcessFunc)GetProcAddress(hKernel32, "TerminateProcess");
    if (OriginalTerminateProcess != NULL) {
      PatchDetour(&OriginalTerminateProcess, reinterpret_cast<PVOID>(TerminateProcessHook), "TerminateProcess");
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] TerminateProcess hook installed (prevents self-termination)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find TerminateProcess");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load kernel32.dll for crash reporter hooks");
  }
}

// ============================================================================
// CrashExceptionFilter instrumentation — why did the game decide to crash?
// ============================================================================
//
// The game prints "=== System Info ===" only from WriteCrashSystemInfo, reachable
// only from HandleCrashDump, reachable only from CrashExceptionFilter
// (ReVault-verified call chain). So that banner in a server log means the game
// entered its crash path — but it says nothing about WHY, and a WINEDEBUG=+seh
// trace of a failing run showed NO access violation and no unhandled exception.
//
// Instrument the entry point itself and report the exception record the game was
// handed. Uses VehPrintf, not Log(): this may run from an exception context
// (N70).
typedef INT64 (*CrashExceptionFilterFunc)(void*, void*, void**);
static CrashExceptionFilterFunc OriginalCrashExceptionFilter = nullptr;

static INT64 CrashExceptionFilterHook(void* a1, void* a2, void** ppExRecords) {
  VehPrintf("[NEVR.CRASH] CrashExceptionFilter ENTERED ret=%p tid=%lu",
            __builtin_return_address(0), GetCurrentThreadId());
  return OriginalCrashExceptionFilter(a1, a2, ppExRecords);
}

// HandleCrashDump — the function that actually prints "=== System Info ===" (via
// WriteCrashSystemInfo). ReVault reports its only static caller is
// CrashExceptionFilter, but a probe on that filter did NOT fire while the banner
// still appeared, which means HandleCrashDump is reached indirectly. Hook it
// directly and report the return address so the real caller is named rather than
// guessed.
typedef INT64 (*HandleCrashDumpFunc)(void*, void*, void*, void*);
static HandleCrashDumpFunc OriginalHandleCrashDump = nullptr;

static INT64 HandleCrashDumpHook(void* a1, void* a2, void* a3, void* a4) {
  const DWORD64 base = reinterpret_cast<DWORD64>(EchoVR::g_GameBaseAddress);
  const DWORD64 ret = reinterpret_cast<DWORD64>(__builtin_return_address(0));
  VehPrintf("[NEVR.CRASH] HandleCrashDump ENTERED ret=0x%llX rva=%s0x%llX tid=%lu "
            "a1=%p a2=%p a3=%p a4=%p",
            static_cast<unsigned long long>(ret),
            (ret >= base && ret < base + 0x2000000) ? "game+" : "abs:",
            static_cast<unsigned long long>((ret >= base && ret < base + 0x2000000) ? ret - base : ret),
            GetCurrentThreadId(), a1, a2, a3, a4);
  // a3 is EXCEPTION_POINTERS** (the caller at 0x1401CEE70 is the
  // SetUnhandledExceptionFilter callback: it spills RCX to the stack and passes
  // its address). Decode it to name the actual fault.
  if (a3 != nullptr && IsReadableMemory(a3, sizeof(void*))) {
    PEXCEPTION_POINTERS ep = *static_cast<PEXCEPTION_POINTERS*>(a3);
    if (ep != nullptr && IsReadableMemory(ep, sizeof(EXCEPTION_POINTERS)) &&
        ep->ExceptionRecord != nullptr &&
        IsReadableMemory(ep->ExceptionRecord, sizeof(EXCEPTION_RECORD))) {
      const DWORD64 ea = reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress);
      VehPrintf("[NEVR.CRASH] >>> EXCEPTION code=0x%08lX flags=0x%lX addr=0x%llX rva=%s0x%llX "
                "nparams=%lu p0=0x%llX p1=0x%llX",
                ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionFlags,
                static_cast<unsigned long long>(ea),
                (ea >= base && ea < base + 0x2000000) ? "game+" : "abs:",
                static_cast<unsigned long long>((ea >= base && ea < base + 0x2000000) ? ea - base : ea),
                ep->ExceptionRecord->NumberParameters,
                static_cast<unsigned long long>(ep->ExceptionRecord->NumberParameters > 0
                                                    ? ep->ExceptionRecord->ExceptionInformation[0] : 0),
                static_cast<unsigned long long>(ep->ExceptionRecord->NumberParameters > 1
                                                    ? ep->ExceptionRecord->ExceptionInformation[1] : 0));
      if (ep->ContextRecord != nullptr && IsReadableMemory(ep->ContextRecord, sizeof(CONTEXT))) {
        const DWORD64 rip = ep->ContextRecord->Rip;
        VehPrintf("[NEVR.CRASH] >>> rip=0x%llX rva=%s0x%llX rsp=0x%llX rcx=0x%llX rdx=0x%llX",
                  static_cast<unsigned long long>(rip),
                  (rip >= base && rip < base + 0x2000000) ? "game+" : "abs:",
                  static_cast<unsigned long long>((rip >= base && rip < base + 0x2000000) ? rip - base : rip),
                  ep->ContextRecord->Rsp, ep->ContextRecord->Rcx, ep->ContextRecord->Rdx);
      }
    } else {
      VehPrintf("[NEVR.CRASH] >>> no EXCEPTION_POINTERS (ep=%p) — fatal-error path, not a fault",
                static_cast<void*>(ep));
    }
  }

  // Stack scan for game-code return addresses — names the call chain.
  DWORD64* sp = reinterpret_cast<DWORD64*>(&a1);
  int found = 0;
  for (int i = 0; i < 96 && found < 10; i++) {
    if (!IsReadableMemory(sp + i, 8)) break;
    const DWORD64 v = sp[i];
    if (v >= base && v < base + 0x1800000) {
      VehPrintf("[NEVR.CRASH]   caller#%d game+0x%llX", found,
                static_cast<unsigned long long>(v - base));
      found++;
    }
  }
  return OriginalHandleCrashDump(a1, a2, a3, a4);
}

// N70/N85: the init-time snapshot is taken in InstallVEH, which runs during
// Initialize() — BEFORE modules (ws_bridge, token_auth, platform_compat) and
// plugins load. Without a refresh, a crash inside any of those is attributed to
// nothing, which is exactly what happened while chasing N85. Re-snapshot once
// the module set is final. Still never called from the handler itself.
void RefreshModuleCache() {
  CacheModuleTable();
  Log(EchoVR::LogLevel::Info,
      "[NEVR.CRASH] module cache refreshed count=%ld (post module/plugin load)",
      g_moduleCacheCount);
}

void InstallCrashFilterInstrumentation() {
  void* filt = reinterpret_cast<void*>(EchoVR::g_GameBaseAddress +
                                       PatchAddresses::CRASH_EXCEPTION_FILTER);
  if (memcmp(filt, PatchAddresses::CRASH_EXCEPTION_FILTER_PROLOGUE,
             sizeof(PatchAddresses::CRASH_EXCEPTION_FILTER_PROLOGUE)) == 0) {
    OriginalCrashExceptionFilter = reinterpret_cast<CrashExceptionFilterFunc>(filt);
    PatchDetour(&OriginalCrashExceptionFilter,
                reinterpret_cast<PVOID>(CrashExceptionFilterHook), "CrashExceptionFilter");
  }

  // HandleCrashDump @ 0x1401CEFE0 — prologue read from the live image and
  // validated at install; never blind-write.
  void* hcd = reinterpret_cast<void*>(EchoVR::g_GameBaseAddress + 0x1CEFE0);
  OriginalHandleCrashDump = reinterpret_cast<HandleCrashDumpFunc>(hcd);
  const unsigned char* pb = static_cast<const unsigned char*>(hcd);
  if (PatchDetour(&OriginalHandleCrashDump,
                  reinterpret_cast<PVOID>(HandleCrashDumpHook), "HandleCrashDump")) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.CRASH] hooked name=HandleCrashDump va=0x1401CEFE0 prologue=%02x%02x%02x%02x%02x "
        "(why-did-we-crash probe)",
        pb[0], pb[1], pb[2], pb[3], pb[4]);
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.CRASH] hook failed name=HandleCrashDump");
  }
}

void InstallVEH() {
  // N70: snapshot the module table now, while the loader lock is safe to take.
  // The handler reads this snapshot instead of enumerating (which would deadlock
  // on any fault raised while the loader lock is held).
  CacheModuleTable();

  // N69: reserve stack below the guard page so the overflow handler has room.
  // Covers this thread; the per-frame hook covers each game thread it runs on.
  EnsureStackReserve();

  // Install VEH to handle int3 that fires after our ExitProcess suppression returns
  AddVectoredExceptionHandler(1, BreakpointVEH);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] veh installed handler=BreakpointVEH priority=1 modules_cached=%ld "
      "stack_reserve_bytes=%lu",
      g_moduleCacheCount, static_cast<unsigned long>(kCrashHandlerStackReserve));
}

// POSIX signal handler — initiates shutdown DIRECTLY.
// The prior flag-based approach (set g_shutdownRequested, check per-frame in
// PrecisionSleepWaitHook) loses the race: after SIGINT, the game begins teardown
// before the next frame runs, so PerformGracefulShutdown was never invoked and
// the listening socket survived as a zombie (N13/N38 root cause re-open).
// Uses write() for async-signal-safe diagnostics (not fprintf/Log).
// PerformGracefulShutdown is NOT formally async-signal-safe (calls Log, GetProcAddress,
// etc.), but called directly from here because the flag alternative is proven broken:
// the per-frame check never fires after signal delivery. Re-entrant-safe via
// InterlockedExchange gate inside PerformGracefulShutdown.
static void PosixSignalHandler(int sig) {
  if (sig == SIGINT) {
    write(STDERR_FILENO, "[NEVR.PATCH] SIGINT received — direct shutdown\n", 47);
  } else if (sig == SIGTERM) {
    write(STDERR_FILENO, "[NEVR.PATCH] SIGTERM received — direct shutdown\n", 48);
  } else {
    write(STDERR_FILENO, "[NEVR.PATCH] signal received — direct shutdown\n", 45);
  }
  g_inSignalContext = 1;  // N62: switches reporting to the async-signal-safe path
  PerformGracefulShutdown(0);
  // Unreachable — PerformGracefulShutdown calls ForceFatalExit which terminates.
}

// ---------------------------------------------------------------------------
// N87 — CTRL+C shutdown under Wine.
//
// Measured, Wine 11.13, GUI-subsystem PE launched as `wine ./x.exe` from a tty
// with NO console at all (GetConsoleWindow() == NULL, and
// AttachConsole(ATTACH_PARENT_PROCESS) fails with ERROR_INVALID_HANDLE):
//
//   * a tty SIGINT to the process group IS delivered to SetConsoleCtrlHandler
//     handlers as CTRL_C_EVENT. No console is required, and none can be
//     obtained. Attaching one is neither possible nor necessary.
//   * the CRT signal(SIGINT) handler is NOT called. Wine does not route the
//     event through the CRT signal table.
//   * handlers run in reverse registration order and the first one returning
//     TRUE ends the chain.
//
// Consequence for this process: InstallConsoleCtrlHandler() runs during
// Initialize(), long before the game installs its own handler, so ours sits
// BEHIND the game's. The game's handler logs "Console close signal received",
// runs a complete and correct server teardown (unregisters the lobby, closes
// the ServerDB WebSocket), and returns TRUE — so ours never ran and no
// "[NEVR.PATCH]" shutdown line was ever emitted. The game then faults in its
// client-side teardown after that point and exits 5 via its crash-dump path.
//
// The fix is therefore NOT to take the signal away from the game (its teardown
// is the graceful part) but to sit in FRONT of it, report, let it run, and
// exit cleanly the moment its server-visible work is done — see
// RearmConsoleCtrlHandler() and GameServerLib::Terminate().
// ---------------------------------------------------------------------------

static void ShutdownReport(const char* fmt, ...);

// Set by ConsoleCtrlHandler when a CTRL+C/close/break event arrives.
//
// Deliberately NOT g_shutdownRequested: that flag is polled by
// NetGameSwitchStateHook (state_machine.cpp:30) and by the per-frame hook
// (wave0_instrumentation.cpp:365), and both call PerformGracefulShutdown the
// instant they see it. The game's own teardown switches NetGame state as its
// FIRST step, so setting g_shutdownRequested here would force-exit the process
// before the lobby is unregistered — the precise opposite of graceful.
static volatile LONG s_consoleShutdownPending = 0;

// Non-zero once RearmConsoleCtrlHandler() has moved us to the front of the
// chain, which is also the point at which we know there is a handler BEHIND us
// to hand the event to. Before that, returning FALSE would hand the event to
// nobody, so we must do the shutdown ourselves.
static volatile LONG s_deferToGameTeardown = 0;

// Upper bound on the game's own teardown. Measured at ~3.1s from
// "Console close signal received" to "Terminated game server"
// (/var/tmp/work-nevr-runtime/n13-run.log, 10:11:51.280 -> 10:11:53.391).
// If it ever exceeds this we must still die: a hung shutdown is worse than an
// ugly one, and a server blocked forever is the failure mode of N4.
static constexpr DWORD kGameTeardownWatchdogMs = 20000;
static HANDLE s_shutdownWatchdogEvent = nullptr;

static DWORD WINAPI ShutdownWatchdogThread(LPVOID) {
  if (WaitForSingleObject(s_shutdownWatchdogEvent, kGameTeardownWatchdogMs) == WAIT_TIMEOUT) {
    ShutdownReport("[NEVR.PATCH] shutdown watchdog expired after %lu ms — the game teardown never "
                   "reached GameServerLib::Terminate; forcing exit",
                   static_cast<unsigned long>(kGameTeardownWatchdogMs));
    PerformGracefulShutdown(1);
    // Unreachable — PerformGracefulShutdown calls ForceFatalExit.
  }
  return 0;
}

// Teardown path for this thread: it never outlives the process. Either the
// deferred shutdown reaches GameServerLib::Terminate and TerminateProcess kills
// it, or the wait times out and it terminates the process itself.
static void StartShutdownWatchdog() {
  s_shutdownWatchdogEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (s_shutdownWatchdogEvent == nullptr) {
    ShutdownReport("[NEVR.PATCH] shutdown watchdog NOT armed (CreateEvent failed err=%lu) — a "
                   "stuck game teardown will not be force-exited",
                   GetLastError());
    return;
  }
  HANDLE thread = CreateThread(nullptr, 0, ShutdownWatchdogThread, nullptr, 0, nullptr);
  if (thread == nullptr) {
    ShutdownReport("[NEVR.PATCH] shutdown watchdog NOT armed (CreateThread failed err=%lu) — a "
                   "stuck game teardown will not be force-exited",
                   GetLastError());
    return;
  }
  CloseHandle(thread);
  ShutdownReport("[NEVR.PATCH] shutdown watchdog armed timeout_ms=%lu",
                 static_cast<unsigned long>(kGameTeardownWatchdogMs));
}

bool ConsoleShutdownPending() { return s_consoleShutdownPending != 0; }

static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
  if (dwCtrlType != CTRL_C_EVENT && dwCtrlType != CTRL_CLOSE_EVENT &&
      dwCtrlType != CTRL_BREAK_EVENT) {
    return FALSE;
  }

  if (InterlockedExchange(&s_consoleShutdownPending, 1) != 0) {
    // Second CTRL+C. The operator is telling us the first one is stuck; stop
    // waiting on the game and go.
    ShutdownReport("[NEVR.PATCH] shutdown signal received again (ctrl_type=%lu) — teardown already "
                   "in progress, forcing exit now",
                   dwCtrlType);
    PerformGracefulShutdown(1);
    return TRUE;
  }

  ShutdownReport("[NEVR.PATCH] shutdown signal received — console ctrl event %lu "
                 "(CTRL+C; a tty SIGINT arrives here under Wine) defer_to_game=%ld",
                 dwCtrlType, static_cast<long>(s_deferToGameTeardown));

  if (s_deferToGameTeardown != 0) {
    // Return FALSE so the chain continues to the game's own handler, which is
    // what actually unregisters the lobby and closes the ServerDB socket. We
    // exit cleanly at the end of GameServerLib::Terminate(), once that work is
    // provably done.
    StartShutdownWatchdog();
    ShutdownReport("[NEVR.PATCH] deferring to the game's console handler for lobby unregistration "
                   "and ServerDB close; clean exit follows at GameServerLib::Terminate");
    return FALSE;
  }

  // Nobody behind us in the chain yet — do it ourselves.
  PerformGracefulShutdown(0);
  // Unreachable — PerformGracefulShutdown calls ForceFatalExit.
  return TRUE;
}

void RearmConsoleCtrlHandler() {
  // Remove-then-add moves us to the front of the LIFO chain without leaving a
  // duplicate entry behind. Call this only from a site that runs AFTER the game
  // has installed its own handler.
  SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
  if (SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE) == FALSE) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] console ctrl handler re-arm FAILED err=%lu — CTRL+C will be consumed by the "
        "game's handler and our shutdown will not report",
        GetLastError());
    return;
  }
  InterlockedExchange(&s_deferToGameTeardown, 1);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] console ctrl handler re-armed to front of chain (CTRL+C reaches us first; the "
      "game's teardown runs behind us)");
}

void InstallConsoleCtrlHandler() {
  // Installed here, this handler sits BEHIND the game's (registered later, and
  // the chain is LIFO). It only becomes reachable once RearmConsoleCtrlHandler()
  // moves it to the front. Registering now still matters: it covers a CTRL+C
  // that arrives before the game has installed its own handler, where
  // s_deferToGameTeardown is 0 and we shut down directly.
  if (SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE) == FALSE) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] SetConsoleCtrlHandler FAILED err=%lu",
        GetLastError());
  } else {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] Console ctrl handler installed (behind the game's until re-armed)");
  }

  // Register POSIX signal handlers too.
  // MEASURED (N87, Wine 11.13): signal(SIGINT, ...) registers successfully but
  // is NEVER called — Wine delivers a tty SIGINT to console ctrl handlers, not
  // to the CRT signal table. These registrations are retained for native
  // Windows and for a hosted SIGTERM, not because they fire under Wine.
  //
  // These handlers call PerformGracefulShutdown DIRECTLY — the prior flag-based
  // approach (set g_shutdownRequested, check per-frame) lost the race to game
  // teardown; the per-frame check never ran after signal delivery (N13/N38 re-open).
  if (signal(SIGINT, PosixSignalHandler) == SIG_ERR) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to register SIGINT handler");
  }
  if (signal(SIGTERM, PosixSignalHandler) == SIG_ERR) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to register SIGTERM handler");
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] POSIX signal handlers installed (SIGINT/SIGTERM -> direct shutdown)");
}

void ServerFatal(const CHAR* format, ...) {
  char buf[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  // These conditions are ONLY fatal in server mode where no human is watching
  // the screen. In client mode, log as a Warning and let the user see it in
  // the game's console output — a degraded hook or missing config won't
  // silently break their session.
  if (!g_isServer) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.FATAL] (non-fatal, client mode) %s", buf);
    return;
  }

  Log(EchoVR::LogLevel::Error, "[NEVR.FATAL] %s", buf);
  ForceFatalExit(1);
}

void ForceFatalExit(unsigned int code) {
  // ExitProcessHook suppresses ALL ExitProcess in server mode to survive the game's
  // crash-reporter chain. Lift it for THIS exit: set g_forceExitInProgress so the
  // intentional ExitProcess — and every ExitProcess the OS/CRT re-enters during
  // termination — passes through to the real kernel32 ExitProcess. (Calling
  // OriginalExitProcess directly is NOT enough: the termination it kicks off
  // re-enters the still-active hook from game threads, which re-suppresses, and the
  // process spins on suppressed ExitProcess forever instead of dying.)
  Log(EchoVR::LogLevel::Error, "[NEVR.PATCH] ForceFatalExit(%u) — terminating process", code);
  // Lift the ExitProcess suppression in case anything re-enters it during teardown.
  g_forceExitInProgress = true;
  // TerminateProcess kills all threads immediately with NO DLL detach / atexit, so it
  // can't hang on a stuck thread or deadlock the loader the way ExitProcess does under
  // Wine (which spins/hangs on termination). Call the real kernel32 TerminateProcess
  // directly to bypass the crash-reporter TerminateProcess hook.
  HANDLE self = GetCurrentProcess();
  if (OriginalTerminateProcess != nullptr) {
    OriginalTerminateProcess(self, code);
  }
  TerminateProcess(self, code);  // fallback (hook allows self-terminate outside crash window)
  ExitProcess(code);             // last resort
}

/// Server-mode fatal error handler — logs to the structured logger and terminates
/// via ForceFatalExit. NEVER blocks on a modal dialog (no MessageBoxA).
/// Replaces the default FatalError behavior (MessageBoxA + exit(1)) when the
/// dedicated server must be absolutely non-interactive.
static VOID ServerFatalErrorHandler(const CHAR* msg, const CHAR* title) {
  Log(EchoVR::LogLevel::Error, "[FATAL] %s: %s", title ? title : "Echo Relay: Error",
      msg ? msg : "An unknown error occurred.");
  // ForceFatalExit sets g_forceExitInProgress to lift the ExitProcess suppression,
  // then calls the real TerminateProcess directly. The server dies immediately
  // with a nonzero exit code so a supervisor (systemd, Docker, watchdog) can
  // restart it — never a silent hang.
  ForceFatalExit(1);
}

void InstallFatalErrorHandler() {
  SetFatalErrorHandler(ServerFatalErrorHandler);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Fatal error handler installed — server will exit on fatal errors");
}

// N62: WsBridge_Shutdown resolved ONCE at init. The signal path must never call
// GetModuleHandleA/GetProcAddress — both serialise on the loader lock, and a
// signal delivered while any thread holds it would deadlock the shutdown.
typedef void (*WsBridgeShutdownFn)(void);
static WsBridgeShutdownFn g_wsBridgeShutdown = nullptr;

void ResolveShutdownDependencies() {
  HMODULE hWsBridge = GetModuleHandleA("ws_bridge.dll");
  if (hWsBridge != nullptr) {
    g_wsBridgeShutdown =
        reinterpret_cast<WsBridgeShutdownFn>(GetProcAddress(hWsBridge, "WsBridge_Shutdown"));
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] shutdown deps resolved ws_bridge=%s WsBridge_Shutdown=%s "
      "(pre-resolved so the signal path takes no loader lock, N62)",
      hWsBridge ? "loaded" : "absent", g_wsBridgeShutdown ? "found" : "null");
}

// N62: report from the shutdown path using a transport that is safe for the
// context we are actually in. Log() reaches vfprintf, which takes the stderr
// FILE lock — a signal delivered while the interrupted thread held that lock
// deadlocks the handler on it. VehPrintf uses only a fixed stack buffer plus
// WriteFile, so it is safe from a signal handler.
static void ShutdownReport(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (g_inSignalContext) {
    VehPrintf("%s", buf);
  } else {
    Log(EchoVR::LogLevel::Info, "%s", buf);
  }
}

void PerformGracefulShutdown(unsigned int exitCode) {
  // Prevent re-entry: if the POSIX signal handler fires while another thread
  // is already running this sequence, only the first call executes. The
  // second call arrives after TerminateProcess — unreachable.
  // This gate also handles the (now-dead-code) per-frame / per-transition
  // flag checks in PrecisionSleepWaitHook and NetGameSwitchStateHook.
  static volatile LONG s_shuttingDown = 0;
  if (InterlockedExchange(&s_shuttingDown, 1) != 0) {
    // N63: re-entry — another signal arrived while shutdown was in
    // progress. The first shutdown may be stuck (deadlocked on N62
    // signal-unsafe locks). Don't return (the caller PosixSignalHandler
    // would return without terminating the process); force-exit now.
    ForceFatalExit(1);
  }

  ShutdownReport("[NEVR.PATCH] Graceful shutdown initiated (code=%u signal_ctx=%d)",
                 exitCode, static_cast<int>(g_inSignalContext));

  // 1. Stop the ws_bridge listener — this is the critical step that releases
  //    the socket FD, preventing the wineserver from holding port 6821 as a
  //    zombie LISTEN socket after the process exits (N38 root cause).
  {
    // N62: use the pointer resolved at init — NO GetModuleHandleA/GetProcAddress
    // here. Both take the loader lock; a signal arriving while another thread
    // holds it would deadlock this shutdown permanently.
    if (g_wsBridgeShutdown != nullptr) {
      ShutdownReport("[NEVR.PATCH] Stopping ws_bridge listener...");
      g_wsBridgeShutdown();
      ShutdownReport("[NEVR.PATCH] ws_bridge listener stopped — socket released");
    } else {
      ShutdownReport("[NEVR.PATCH] ws_bridge shutdown unavailable (not loaded at init) "
                     "— listener may leak");
    }
  }

  // 2. Unhook MinHook hooks installed by Wave0 instrumentation.
  Wave0::Shutdown();

  // 3. Force exit (bypasses server-mode ExitProcess suppression).
  //    ForceFatalExit sets g_forceExitInProgress, then calls
  //    OriginalTerminateProcess which kills all threads immediately.
  //    On Wine, this also terminates the wineserver association, so the
  //    listener socket released in step 1 is reclaimed by the kernel.
  ForceFatalExit(exitCode);
}

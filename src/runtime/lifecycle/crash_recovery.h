#pragma once

#include "core/pch.h"

/// Installs CreateProcessA, CreateProcessW, ExitProcess, and TerminateProcess hooks
/// to suppress the BugSplat crash reporter and prevent crash-triggered termination.
void InstallCrashRecoveryHooks();

/// Installs the BreakpointVEH that handles int3 after suppressed ExitProcess
/// and null-pointer AV recovery via longjmp in server mode.
/// Also snapshots the module table (N70 — the handler must never enumerate,
/// which would take the loader lock) and reserves crash-handler stack for the
/// calling thread (N69).
void InstallVEH();

/// Hooks the game's CrashExceptionFilter to report WHY the game entered its crash
/// path. Purely diagnostic — always calls the original.
void InstallCrashFilterInstrumentation();

/// Re-snapshot the module table after modules and plugins have loaded, so crash
/// frames inside them can be attributed. Must be called from normal code, never
/// from a handler (it takes the loader lock).
void RefreshModuleCache();

/// N62: resolve WsBridge_Shutdown once, at init, so the signal-delivered shutdown
/// path never calls GetModuleHandleA/GetProcAddress (both take the loader lock).
/// Must run AFTER modules load.
void ResolveShutdownDependencies();

/// N69: reserve stack below the guard page so the stack-overflow handler has room
/// to run. Per-thread and idempotent (thread_local one-shot) — call it from any
/// hook that runs on a thread which could fault. Cheap after the first call on a
/// given thread: a single thread_local bool test.
void EnsureStackReserve();

/// Installs the console ctrl handler so CTRL+C actually terminates the process.
/// N87: under Wine a tty SIGINT is delivered here (as CTRL_C_EVENT) and NOT to
/// the CRT signal table, with or without a console. Installed this early the
/// handler sits BEHIND the game's own — see RearmConsoleCtrlHandler.
void InstallConsoleCtrlHandler();

/// N87: moves our console ctrl handler to the front of the LIFO handler chain.
/// Console handlers run in reverse registration order and the first to return
/// TRUE ends the chain; the game installs its own handler after ours and returns
/// TRUE, so without this ours never runs. Call only from a site that runs after
/// the game is fully initialised (GameServerLib::Initialize) — before that there
/// is no handler behind us to hand the event to.
void RearmConsoleCtrlHandler();

/// N87: TRUE once a console CTRL+C/close/break event has been observed. Read by
/// GameServerLib::Terminate to exit cleanly at the end of the game's own
/// teardown, instead of letting the game fault in the client-side teardown that
/// follows it. Distinct from g_shutdownRequested, which triggers an immediate
/// PerformGracefulShutdown from the per-frame and per-state-transition hooks and
/// would therefore kill the process before the lobby is unregistered.
bool ConsoleShutdownPending();

/// Forces a genuine process exit with the given code, bypassing the server-mode
/// ExitProcess suppression in ExitProcessHook. Uses the real kernel32 ExitProcess
/// captured before our hook (the same mechanism InstallConsoleCtrlHandler uses to
/// terminate in server mode), with TerminateProcess/_exit fallbacks. For fail-loud
/// conditions where the server MUST die rather than run silently degraded.
void ForceFatalExit(unsigned int code);

/// Installs a fatal-error handler via SetFatalErrorHandler that routes fatal errors
/// through a structured log + ForceFatalExit instead of blocking on MessageBoxA.
/// Call AFTER g_isServer is known, BEFORE any code path that can hit FatalError
/// (specifically, before LoadModule calls in PreprocessCommandLineHook).
/// In client mode the handler is NOT installed — users at the screen should see
/// the modal dialog so they know the game fatally failed.
void InstallFatalErrorHandler();

/// Initiates graceful shutdown: (1) stops the ws_bridge listener (releases the
/// socket FD to prevent wineserver zombie — N38 root cause), (2) unhooks BinaryBugFixes
/// MinHook hooks, (3) calls ForceFatalExit to terminate the process.
/// Safe to call from any thread — uses InterlockedExchange to prevent re-entry.
/// Primary call sites: POSIX signal handler (SIGINT/SIGTERM — direct call, NOT
/// flag-deferred), SetConsoleCtrlHandler lambda (direct), per-frame
/// PrecisionSleepWaitHook (flag check, now dead-code defense), per-transition
/// NetGameSwitchStateHook (flag check + session-end path).
void PerformGracefulShutdown(unsigned int exitCode);

/// Server-mode fatal exit — logs the cause as the last log line and immediately
/// terminates via ForceFatalExit. In client mode (g_isServer=FALSE) the condition
/// is logged as a Warning and execution continues — these conditions are only
/// fatal when the server, not a human at the screen, would otherwise run degraded
/// for hours.
/// Call ONLY after g_isServer is known (i.e., after early -server detection in
/// PreprocessCommandLineHook). For code that runs earlier (Initialize/DllMain),
/// set a deferred flag and check it after handler installation.
void ServerFatal(const CHAR* format, ...);

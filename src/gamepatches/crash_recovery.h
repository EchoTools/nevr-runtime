#pragma once

#include "common/pch.h"

/// Installs CreateProcessA, CreateProcessW, ExitProcess, and TerminateProcess hooks
/// to suppress the BugSplat crash reporter and prevent crash-triggered termination.
void InstallCrashRecoveryHooks();

/// Installs the BreakpointVEH that handles int3 after suppressed ExitProcess
/// and null-pointer AV recovery via longjmp in server mode.
void InstallVEH();

/// Installs the console ctrl handler so CTRL+C actually terminates the process.
void InstallConsoleCtrlHandler();

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
/// socket FD to prevent wineserver zombie — N38 root cause), (2) unhooks Wave0
/// MinHook hooks, (3) calls ForceFatalExit to terminate the process.
/// Safe to call from any thread — uses InterlockedExchange to prevent re-entry.
/// Called from the per-frame PrecisionSleepWaitHook (flag check), the
/// per-transition NetGameSwitchStateHook (flag check + session-end path),
/// and the POSIX signal handler path (via flag → next tick check).
void PerformGracefulShutdown(unsigned int exitCode);

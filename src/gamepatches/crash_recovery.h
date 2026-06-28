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

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

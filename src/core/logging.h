#pragma once

#include <chrono>
#include <ctime>
#include <string>

#include "abi/echovr_functions.h"

/// N80: stable per-process run identifier, shared by every log writer in the
/// process so lines from different files can be correlated to one run.
///
/// Derived from the PID and the process creation time — not from a counter or a
/// call-time clock — so BootLogTee (which writes <exedir>\logs\nevr-boot.jsonl,
/// append-mode across runs) and the log filter (which writes a per-run file under
/// %LOCALAPPDATA%) independently compute the SAME value. Without it the two files
/// cannot be joined, and the append-mode boot log cannot even be split by run.
///
/// Returns a stable pointer to a static buffer. Safe to call from DllMain: no heap,
/// no loader-lock call, no CRT locale dependency.
extern const CHAR* GetRunId();

// Helper function to get log level string
extern const char* GetLogLevelString(EchoVR::LogLevel level);

/// The single log entry point. It routes by boot phase: while a boot sink is set it reports through
/// that sink, otherwise through the game's CLog (EchoVR::WriteLog), or stderr before that is resolved.
/// No caller branches on the phase.
extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

/// Where Log() reports during the runtime's boot. The boot runs under the DllMain loader lock, where
/// the game's CLog is not usable (#495), so nevr_boot_log_tee::Init() sets the sink to the boot tee
/// and nevr_boot_log_tee::Close() clears it: the boot phase is exactly the window a sink is set. The
/// sink lives here, not in src/runtime/, because src/core/ does not depend on the runtime. It gets
/// the formatted message.
typedef VOID (*BootLogSink)(EchoVR::LogLevel level, const CHAR* message);
/// Sets the boot sink; nullptr clears it and returns Log() to the game's CLog.
void SetBootLogSink(BootLogSink sink);

extern VOID FatalError(const CHAR* msg, const CHAR* title);

/// Typedef for a fatal error handler — called by FatalError when registered,
/// before MessageBoxA/exit. The handler is responsible for logging and terminating
/// the process. Registered via SetFatalErrorHandler (typically by gamepatches when
/// booting in server mode to prevent blocking on a modal dialog).
typedef VOID (*FatalErrorHandlerFunc)(const CHAR* msg, const CHAR* title);
extern FatalErrorHandlerFunc g_fatalErrorHandler;
void SetFatalErrorHandler(FatalErrorHandlerFunc handler);

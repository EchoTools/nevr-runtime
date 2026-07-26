#pragma once

#include <chrono>
#include <ctime>
#include <string>

#include "echovr_functions.h"

// Helper function to get ISO8601 timestamp
extern std::string GetISO8601Timestamp();

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

// Helper function to format JSON log entry
extern std::string FormatJsonLogEntry(EchoVR::LogLevel level, const char* message, const char* caller = nullptr);

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

extern VOID FatalError(const CHAR* msg, const CHAR* title);

/// Typedef for a fatal error handler — called by FatalError when registered,
/// before MessageBoxA/exit. The handler is responsible for logging and terminating
/// the process. Registered via SetFatalErrorHandler (typically by gamepatches when
/// booting in server mode to prevent blocking on a modal dialog).
typedef VOID (*FatalErrorHandlerFunc)(const CHAR* msg, const CHAR* title);
extern FatalErrorHandlerFunc g_fatalErrorHandler;
void SetFatalErrorHandler(FatalErrorHandlerFunc handler);

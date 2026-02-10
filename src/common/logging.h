#pragma once
#include "echovrInternal.h"

#ifndef ECHOVR_LOGGING_H
#define ECHOVR_LOGGING_H

#include <chrono>
#include <ctime>
#include <string>

// Helper function to get ISO8601 timestamp
extern std::string GetISO8601Timestamp();

// Helper function to get log level string
extern const char* GetLogLevelString(EchoVR::LogLevel level);

// Helper function to format JSON log entry
extern std::string FormatJsonLogEntry(EchoVR::LogLevel level, const char* message, const char* caller = nullptr);

extern VOID __cdecl WriteLogHook(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl);

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

extern VOID FatalError(const CHAR* msg, const CHAR* title);

#endif  // ECHOVR_LOGGING_H
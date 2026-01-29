#pragma once
#include "platform_stubs.h"

// Minimal forward declarations to avoid pulling in the full extern headers for LSP diagnostics.
namespace EchoVR {
enum class LogLevel : INT32 { Debug = 0, Info = 1, Warning = 2, Error = 3, Default = 4, Any = 5 };
extern VOID WriteLog(LogLevel level, UINT64 unk, const CHAR* format, va_list vl);
}  // namespace EchoVR

#ifndef ECHOVR_LOGGING_H
#define ECHOVR_LOGGING_H

extern VOID __cdecl WriteLogHook(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl);

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

extern VOID FatalError(const CHAR* msg, const CHAR* title);

#endif  // ECHOVR_LOGGING_H

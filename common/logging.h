#pragma once
#include "echovr_internal.h"

#ifndef ECHOVR_LOGGING_H
#define ECHOVR_LOGGING_H

extern VOID __cdecl WriteLogHook(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl);

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

extern VOID FatalError(const CHAR* msg, const CHAR* title);

#endif  // ECHOVR_LOGGING_H

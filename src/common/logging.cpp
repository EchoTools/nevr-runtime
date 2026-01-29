#include "logging.h"

#include <stdio.h>
#include <string.h>

#include "globals.h"
#include "platform_stubs.h"

VOID WriteLogHook(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl) {
  if (!strcmp(format, "[DEBUGPRINT] %s %s") || !strcmp(format, "[SCRIPT] %s: %s")) {
    // If the overall template matched, format it
    CHAR formattedLog[0x1000];
    memset(formattedLog, 0, sizeof(formattedLog));
    va_list vl_copy;
    va_copy(vl_copy, vl);
    vsprintf_s(formattedLog, format, vl_copy);
    va_end(vl_copy);

    // If the final output matches the strings below, we do not log.
    if (!strcmp(formattedLog,
                "[DEBUGPRINT] PickRandomTip: context = 0x41D2C432172E0810"))  // noisy in main menu / loading screen
      return;
    if (!strcmp(formattedLog, "[SCRIPT] 0xA9DB89899292A98F: realdiv(d9a3e735) divide by zero"))  // laggy in game
      return;
  } else if (!strcmp(format, "[NETGAME] No screen stats info for game mode %s"))  // noisy in social lobby
    return;

  // Calling the original function and returning here if noConsole is set to avoid putting any extra formatting in the
  // logs.
  if (noConsole) return EchoVR::WriteLog(logLevel, unk, format, vl);

  // Print the ANSI color code prefix for the given log level.
  switch (logLevel) {
    case EchoVR::LogLevel::Debug:
      printf("\u001B[36m");
      break;

    case EchoVR::LogLevel::Warning:
      printf("\u001B[33m");
      break;

    case EchoVR::LogLevel::Error:
      printf("\u001B[31m");
      break;

    case EchoVR::LogLevel::Info:
    default:
      printf("\u001B[0m");
      break;
  }

  // Print the output to our allocated console.
  va_list vl_copy2;
  va_copy(vl_copy2, vl);
  vprintf(format, vl_copy2);
  va_end(vl_copy2);
  // Only print a newline if the format string does not already end with one.
  size_t formatLen = strlen(format);
  if (formatLen == 0 || format[formatLen - 1] != '\n') {
    printf("\n");
  }

  // Print the ANSI color code for restoring the default text style.
  printf("\u001B[0m");

  // Call the original method
  EchoVR::WriteLog(logLevel, unk, format, vl);
}

VOID Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
  if (isHeadless)
    WriteLogHook(level, 0, format, args);
  else
    EchoVR::WriteLog(level, 0, format, args);
  va_end(args);
}

VOID FatalError(const CHAR* msg, const CHAR* title) {
  // If no title or msg was provided, set it to a generic value.
  if (title == NULL) title = "Echo Relay: Error";
  if (msg == NULL) msg = "An unknown error occurred.";

  // Show a message box.
  MessageBoxA(NULL, msg, title, MB_OK);

  // Force process exit with an error code.
  exit(1);
}

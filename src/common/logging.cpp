#include "logging.h"

#include <json/json.h>

#include <iomanip>
#include <sstream>

#include "echovr.h"
#include "echovrInternal.h"
#include "globals.h"

// Get ISO8601 timestamp in UTC
std::string GetISO8601Timestamp() {
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::tm tm_utc;
#ifdef _WIN32
  gmtime_s(&tm_utc, &now_time_t);
#else
  gmtime_r(&now_time_t, &tm_utc);
#endif

  std::ostringstream oss;
  oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << now_ms.count()
      << 'Z';
  return oss.str();
}

// Convert LogLevel enum to string
const char* GetLogLevelString(EchoVR::LogLevel level) {
  switch (level) {
    case EchoVR::LogLevel::Debug:
      return "debug";
    case EchoVR::LogLevel::Info:
      return "info";
    case EchoVR::LogLevel::Warning:
      return "warn";
    case EchoVR::LogLevel::Error:
      return "error";
    default:
      return "info";
  }
}

// Format a JSON log entry similar to zap.Logger
std::string FormatJsonLogEntry(EchoVR::LogLevel level, const char* message, const char* caller) {
  Json::Value root;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";  // Compact JSON, no pretty-printing

  root["level"] = GetLogLevelString(level);
  root["ts"] = GetISO8601Timestamp();
  root["msg"] = message;

  if (caller != nullptr) {
    root["caller"] = caller;
  }

  return Json::writeString(builder, root);
}

VOID WriteLogHook(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl) {
  if (!strcmp(format, "[DEBUGPRINT] %s %s") || !strcmp(format, "[SCRIPT] %s: %s")) {
    CHAR formattedLog[0x1000];
    memset(formattedLog, 0, sizeof(formattedLog));
    va_list vl_copy;
    va_copy(vl_copy, vl);
#ifdef _WIN32
    vsprintf_s(formattedLog, format, vl_copy);
#else
    vsnprintf(formattedLog, sizeof(formattedLog), format, vl_copy);
#endif
    va_end(vl_copy);

    if (!strcmp(formattedLog, "[DEBUGPRINT] PickRandomTip: context = 0x41D2C432172E0810")) return;
    if (!strcmp(formattedLog, "[SCRIPT] 0xA9DB89899292A98F: realdiv(d9a3e735) divide by zero")) return;
  } else if (!strcmp(format, "[NETGAME] No screen stats info for game mode %s"))
    return;

  if (noConsole) return EchoVR::WriteLog(logLevel, unk, format, vl);

  CHAR formattedMessage[0x1000];
  memset(formattedMessage, 0, sizeof(formattedMessage));
  va_list vl_copy2;
  va_copy(vl_copy2, vl);
#ifdef _WIN32
  vsprintf_s(formattedMessage, format, vl_copy2);
#else
  vsnprintf(formattedMessage, sizeof(formattedMessage), format, vl_copy2);
#endif
  va_end(vl_copy2);

  std::string jsonLog = FormatJsonLogEntry(logLevel, formattedMessage);
  printf("%s\n", jsonLog.c_str());
  fflush(stdout);

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
  if (title == NULL) title = "Echo Relay: Error";
  if (msg == NULL) msg = "An unknown error occurred.";

#ifdef _WIN32
  MessageBoxA(NULL, msg, title, 0x00000000L);  // MB_OK
#else
  fprintf(stderr, "[FATAL] %s: %s\n", title, msg);
#endif

  exit(1);
}
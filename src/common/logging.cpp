#include "logging.h"

#include <nlohmann/json.hpp>

#include <iomanip>
#include <sstream>

#include "echovr.h"
#include "echovr_functions.h"
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
  nlohmann::json root;
  root["ts"] = GetISO8601Timestamp();
  root["level"] = GetLogLevelString(level);
  root["msg"] = message;

  if (caller != nullptr) {
    root["caller"] = caller;
  }

  return root.dump();
}

// WriteLogHook removed — log filtering, timestamps, and noise suppression
// are now handled by the log_filter plugin via CLog::PrintfImpl hook.

VOID Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
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
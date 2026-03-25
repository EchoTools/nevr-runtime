#include "logging.h"

#include <json/json.h>

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
  Json::Value root;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";  // Compact JSON, no pretty-printing

  root["ts"] = GetISO8601Timestamp();
  root["level"] = GetLogLevelString(level);
  root["msg"] = message;

  if (caller != nullptr) {
    root["caller"] = caller;
  }

  return Json::writeString(builder, root);
}

/// Helper to call WriteLog with variadic args (builds a proper va_list)
static VOID WriteLogV(EchoVR::LogLevel level, UINT64 unk, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
  EchoVR::WriteLog(level, unk, format, args);
  va_end(args);
}

VOID WriteLogHook(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl) {
  // Recursion guard — EchoVR::WriteLog is detoured back to us.
  // Any call to EchoVR::WriteLog or WriteLogV from within this function
  // re-enters the hook. The guard ensures the re-entrant call passes through.
  static thread_local bool inHook = false;
  if (inHook) {
    EchoVR::WriteLog(logLevel, unk, format, vl);
    return;
  }
  struct Guard { bool& f; ~Guard() { f = false; } } guard{inHook};
  inHook = true;

  // Default to info — skip debug-level messages from stdout (still goes to game log)
  if (logLevel == EchoVR::LogLevel::Debug) {
    EchoVR::WriteLog(logLevel, unk, format, vl);
    return;
  }

  // Capture login session GUID from pnsrad.dll's login response.
  // pnsrad.dll prints "[NSUSER] LoginId: <GUID>:" directly (bypasses WriteLog).
  // We match any message containing "LoginId" that goes through WriteLog as a fallback,
  // but the primary capture is in NetGameSwitchStateHook when state transitions to LoggedIn.

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

  EchoVR::WriteLog(logLevel, unk, format, vl);
}

VOID Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);

  // The game's WriteLog puts [ERROR]/[WARNING] BEFORE the timestamp.
  // To keep timestamp-first ordering, downgrade our error/warning messages
  // to Info level and embed the level tag in the message text.
  // Removed: level conversion experiment

  // Always go through WriteLogHook — it handles level-to-timestamp reordering
  // and debug filtering. EchoVR::WriteLog (trampoline) is called at the end.
  WriteLogHook(level, 0, format, args);
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
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

// N80: per-process run ID = PID + process creation time, so every writer in the
// process derives the same value independently. Computed once; the benign race on
// first call has both racers writing identical bytes.
const CHAR* GetRunId() {
  static CHAR s_runId[40] = {0};
  if (s_runId[0] != '\0') return s_runId;

#ifdef _WIN32
  FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
  unsigned long long start = 0;
  if (GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelTime, &userTime)) {
    start = (static_cast<unsigned long long>(createTime.dwHighDateTime) << 32) |
            createTime.dwLowDateTime;
  }
  snprintf(s_runId, sizeof(s_runId), "%lx-%llx",
           static_cast<unsigned long>(GetCurrentProcessId()), start);
#else
  snprintf(s_runId, sizeof(s_runId), "%lx-0", static_cast<unsigned long>(getpid()));
#endif
  return s_runId;
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
  if (EchoVR::WriteLog != nullptr) {
    EchoVR::WriteLog(level, 0, format, args);
  } else {
    // Game logging not available yet (early DllMain init) — fall back to stderr
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
  }
  va_end(args);
}

FatalErrorHandlerFunc g_fatalErrorHandler = nullptr;

void SetFatalErrorHandler(FatalErrorHandlerFunc handler) { g_fatalErrorHandler = handler; }

VOID FatalError(const CHAR* msg, const CHAR* title) {
  if (title == NULL) title = "Echo Relay: Error";
  if (msg == NULL) msg = "An unknown error occurred.";

  // If a fatal-error handler is registered (e.g., by gamepatches for server mode),
  // delegate to it — NEVER block on a modal dialog. The handler is responsible for
  // logging and terminating the process.
  if (g_fatalErrorHandler != nullptr) {
    g_fatalErrorHandler(msg, title);
    return;  // handler must terminate the process — unreachable after ForceFatalExit
  }

#ifdef _WIN32
  MessageBoxA(NULL, msg, title, 0x00000000L);  // MB_OK
#else
  fprintf(stderr, "[FATAL] %s: %s\n", title, msg);
#endif

  exit(1);
}
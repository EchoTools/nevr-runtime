#include "hook_log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace sentinel {

namespace {

std::atomic<LogSink> g_sink{nullptr};

void DefaultSink(LogLevel level, const char* line) {
#ifdef __ANDROID__
  const int priority = level == LogLevel::kError  ? ANDROID_LOG_ERROR
                       : level == LogLevel::kWarn ? ANDROID_LOG_WARN
                                                  : ANDROID_LOG_INFO;
  __android_log_write(priority, "NEVR-Sentinel", line);
#else
  const char* tag = level == LogLevel::kError ? "ERROR" : level == LogLevel::kWarn ? "WARN" : "INFO";
  std::fprintf(stderr, "NEVR-Sentinel %s %s\n", tag, line);
#endif
}

}  // namespace

LogSink SetLogSink(LogSink sink) { return g_sink.exchange(sink, std::memory_order_acq_rel); }

void LogEvent(LogLevel level, const char* format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  const LogSink sink = g_sink.load(std::memory_order_acquire);
  (sink != nullptr ? sink : DefaultSink)(level, line);
}

}  // namespace sentinel

#include "sentinel_log.h"

#include <android/log.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <fcntl.h>
#include <unistd.h>

#ifndef NEVR_QUEST_FILES_DIR
#define NEVR_QUEST_FILES_DIR "/sdcard/Android/data/com.readyatdawn.r15/files"
#endif

#define NEVR_TAG "NEVR-Sentinel"

namespace sentinel {

namespace {

std::mutex g_mutex;
int g_fd = -1;
bool g_failureReported = false;

int Priority(nevr_quest::LogLevel level) {
  switch (level) {
    case nevr_quest::LogLevel::kInfo: return ANDROID_LOG_INFO;
    case nevr_quest::LogLevel::kWarn: return ANDROID_LOG_WARN;
    case nevr_quest::LogLevel::kError: break;
  }
  return ANDROID_LOG_ERROR;
}

const char* LevelName(nevr_quest::LogLevel level) {
  switch (level) {
    case nevr_quest::LogLevel::kInfo: return "INFO";
    case nevr_quest::LogLevel::kWarn: return "WARN";
    case nevr_quest::LogLevel::kError: break;
  }
  return "ERROR";
}

void ReportDiskFailure(const char* what, int err) {
  if (g_failureReported) return;
  g_failureReported = true;
  __android_log_print(ANDROID_LOG_ERROR, NEVR_TAG, "on-disk log %s errno=%d (%s); logcat only",
                      what, err, std::strerror(err));
}

}  // namespace

const char* FilesDir() { return NEVR_QUEST_FILES_DIR; }

void Emit(nevr_quest::LogLevel level, const std::string& message) {
  __android_log_write(Priority(level), NEVR_TAG, message.c_str());

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_fd < 0) {
    const std::string path = std::string(FilesDir()) + "/nevr-sentinel.log";
    g_fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (g_fd < 0) {
      ReportDiskFailure("open failed", errno);
      return;
    }
  }
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  char prefix[64];
  const int n = std::snprintf(prefix, sizeof(prefix), "%lld.%03ld %s ", static_cast<long long>(ts.tv_sec),
                              ts.tv_nsec / 1000000L, LevelName(level));
  std::string line(prefix, n > 0 ? static_cast<std::size_t>(n) : 0);
  line += message;
  line += '\n';
  const ssize_t written = ::write(g_fd, line.data(), line.size());
  if (written != static_cast<ssize_t>(line.size())) {
    ReportDiskFailure("write failed", written < 0 ? errno : EIO);
  }
}

}  // namespace sentinel

#include "sentinel_log.h"

#include <android/log.h>

#include <cerrno>
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

// Constant-initialised: nothing here has a dynamic initializer that could run after the ELF
// constructor that first logs.
std::mutex g_mutex;
int g_fd = -1;
bool g_openFailureReported = false;
bool g_writeFailureReported = false;

int Priority(nevr_quest::LogLevel level) {
  switch (level) {
    case nevr_quest::LogLevel::kInfo: return ANDROID_LOG_INFO;
    case nevr_quest::LogLevel::kWarn: return ANDROID_LOG_WARN;
    case nevr_quest::LogLevel::kError: break;
  }
  return ANDROID_LOG_ERROR;
}

void ReportDiskFailure(const char* what, int err, bool* reported) {
  if (*reported) return;
  *reported = true;
  __android_log_print(ANDROID_LOG_ERROR, NEVR_TAG, "on-disk log %s errno=%d (%s); logcat only", what, err,
                      std::strerror(err));
}

}  // namespace

const char* FilesDir() { return NEVR_QUEST_FILES_DIR; }

void CloseDiskLog() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
}

bool WriteAll(int fd, const char* data, std::size_t size, int* err, WriteFn write) {
  std::size_t done = 0;
  while (done < size) {
    const ssize_t n = write(fd, data + done, size - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      *err = errno;
      return false;
    }
    if (n == 0) {
      *err = EIO;
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

void Emit(nevr_quest::LogLevel level, const std::string& message) {
  __android_log_write(Priority(level), NEVR_TAG, message.c_str());

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_fd < 0) {
    const std::string path = std::string(FilesDir()) + "/nevr-sentinel.log";
    g_fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (g_fd < 0) {
      ReportDiskFailure("open failed", errno, &g_openFailureReported);
      return;
    }
  }
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  const long long unixMs = static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000L;
  const std::string line = nevr_quest::FormatDiskLogLine(level, unixMs, message);
  int err = 0;
  if (!WriteAll(g_fd, line.data(), line.size(), &err, ::write)) {
    ReportDiskFailure("write failed", err, &g_writeFailureReported);
  }
}

}  // namespace sentinel

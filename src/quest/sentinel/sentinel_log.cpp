#include "sentinel_log.h"

#include <android/log.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <mutex>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NEVR_QUEST_FILES_DIR
#define NEVR_QUEST_FILES_DIR "/sdcard/Android/data/com.readyatdawn.r15/files"
#endif

#define NEVR_TAG "NEVR-Sentinel"

namespace sentinel {

namespace {

// These are constant-initialised, so the ELF constructor may log before any dynamic initializer
// in this file has run. std::mutex still registers its destructor with __cxa_atexit from
// _GLOBAL__sub_I_sentinel_log.cpp; that registration comes after the constructor's function-local
// statics (such as activation.cpp's config), so at process exit g_mutex is destroyed first.
// Nothing here logs from an atexit handler.
std::mutex g_mutex;
int g_fd = -1;
bool g_openFailureReported = false;
bool g_nonRegularReported = false;
bool g_writeFailureReported = false;
bool g_tornLine = false;
bool g_diskDisabled = false;  // the path is not a regular file: never retried

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

long long NowUnixMs() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000L;
}

// Renames a log that has reached the size bound; nothing is ever deleted. Best effort: if the
// rename fails the log keeps growing and the next start tries again.
void RotateIfLarge(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < kMaxDiskLogBytes) return;
  const std::string rotated = std::string(FilesDir()) + "/nevr-sentinel." + std::to_string(NowUnixMs()) + ".log";
  if (::rename(path.c_str(), rotated.c_str()) != 0) {
    __android_log_print(ANDROID_LOG_WARN, NEVR_TAG, "on-disk log rotation failed errno=%d (%s)", errno,
                        std::strerror(errno));
  }
}

// g_mutex held. The open is non-blocking and the target must be a regular file: a FIFO at the log
// path must not stall the ELF constructor.
bool OpenDiskLog() {
  const std::string path = std::string(FilesDir()) + "/nevr-sentinel.log";
  RotateIfLarge(path);
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0644);
  if (fd < 0) {
    ReportDiskFailure("open failed", errno, &g_openFailureReported);
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ReportDiskFailure("open failed (fstat)", errno, &g_openFailureReported);
    ::close(fd);
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    g_diskDisabled = true;
    if (!g_nonRegularReported) {
      g_nonRegularReported = true;
      __android_log_write(ANDROID_LOG_ERROR, NEVR_TAG,
                          "on-disk log open failed: path is not a regular file; logcat only");
    }
    return false;
  }
  g_fd = fd;
  return true;
}

}  // namespace

const char* FilesDir() { return NEVR_QUEST_FILES_DIR; }

void EmitFixed(nevr_quest::LogLevel level, const char* literal) noexcept {
  __android_log_write(Priority(level), NEVR_TAG, literal);
}

void CloseDiskLog() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
  g_diskDisabled = false;
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

bool WriteRecord(int fd, const std::string& line, bool* torn, int* err, WriteFn write) {
  const std::string out = *torn ? "\n" + line : line;
  if (WriteAll(fd, out.data(), out.size(), err, write)) {
    *torn = false;
    return true;
  }
  *torn = true;
  return false;
}

void Emit(nevr_quest::LogLevel level, const std::string& message) {
  __android_log_write(Priority(level), NEVR_TAG, message.c_str());

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_diskDisabled) return;
  if (g_fd < 0 && !OpenDiskLog()) return;
  const std::string line = nevr_quest::FormatDiskLogLine(level, NowUnixMs(), message);
  int err = 0;
  if (!WriteRecord(g_fd, line, &g_tornLine, &err, ::write)) {
    ReportDiskFailure("write failed", err, &g_writeFailureReported);
  }
}

}  // namespace sentinel

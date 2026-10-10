#include "sentinel_log.h"

#include <android/log.h>

#include <cerrno>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NEVR_QUEST_FILES_DIR
#define NEVR_QUEST_FILES_DIR "/sdcard/Android/data/com.readyatdawn.r15/files"
#endif

#define NEVR_TAG "NEVR-Sentinel"

namespace sentinel {

namespace {

// Everything here is constant-initialised and trivially destructible: no dynamic initializer and
// no __cxa_atexit registration, so the ELF constructor may log before any initializer in this file
// would have run and nothing here runs at process exit. (A std::mutex would add one atexit
// registration; tools/check_quest_static_init.sh allows none.)
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_fd = -1;
bool g_openFailureReported = false;
bool g_nonRegularReported = false;
bool g_writeFailureReported = false;
bool g_tornLine = false;
bool g_diskDisabled = false;     // the path is not a regular file: never retried
bool g_openFailed = false;       // the last open failed; retried after kOpenRetryMs
long long g_lastOpenFailMs = 0;
long long g_logBytes = 0;        // size of the open log: its size at open plus what this run wrote
unsigned g_openAttempts = 0;
NowFn g_now = nullptr;

class Lock {
 public:
  Lock() { pthread_mutex_lock(&g_mutex); }
  ~Lock() { pthread_mutex_unlock(&g_mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

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

long long NowMs() {
  if (g_now != nullptr) return g_now();
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000L;
}

// Renames a log that has reached the size bound to a name that does not exist yet; nothing is
// ever deleted or replaced. Best effort: on failure the log keeps growing and the next open
// tries again.
void RotateIfLarge(const std::string& path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < kMaxDiskLogBytes) return;
  const std::string base = std::string(FilesDir()) + "/nevr-sentinel." + std::to_string(NowMs());
  for (int n = 0; n < 1000; ++n) {
    const std::string rotated = base + (n == 0 ? "" : "." + std::to_string(n)) + ".log";
    struct stat taken {};
    if (::lstat(rotated.c_str(), &taken) == 0) continue;
    if (::rename(path.c_str(), rotated.c_str()) != 0) {
      __android_log_print(ANDROID_LOG_WARN, NEVR_TAG, "on-disk log rotation failed errno=%d (%s)", errno,
                          std::strerror(errno));
    }
    return;
  }
  __android_log_write(ANDROID_LOG_WARN, NEVR_TAG, "on-disk log rotation skipped: no free name");
}

void FailOpen(int err, const char* what) {
  g_openFailed = true;
  g_lastOpenFailMs = NowMs();
  ReportDiskFailure(what, err, &g_openFailureReported);
}

// g_mutex held. The open is non-blocking, never follows a symlink, and the target must be a
// regular file: a FIFO or device at the log path must not stall the ELF constructor.
bool OpenDiskLog() {
  ++g_openAttempts;
  const std::string path = std::string(FilesDir()) + "/nevr-sentinel.log";
  RotateIfLarge(path);
  const int fd = ::open(path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, 0644);
  if (fd < 0) {
    FailOpen(errno, "open failed");
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const int err = errno;
    ::close(fd);
    FailOpen(err, "open failed (fstat)");
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
  // Regular files are never non-blocking: EAGAIN must not look like a write failure.
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  // A file that does not end in a newline holds a torn record: the next record starts a new line.
  g_tornLine = false;
  if (st.st_size > 0) {
    char last = '\n';
    if (::pread(fd, &last, 1, st.st_size - 1) == 1 && last != '\n') g_tornLine = true;
  }
  g_logBytes = st.st_size;
  g_openFailed = false;
  g_fd = fd;
  return true;
}

}  // namespace

const char* FilesDir() { return NEVR_QUEST_FILES_DIR; }

void SetClockForTest(NowFn now) { g_now = now; }
unsigned OpenAttemptsForTest() { return g_openAttempts; }

void EmitFixed(nevr_quest::LogLevel level, const char* literal) noexcept {
  __android_log_write(Priority(level), NEVR_TAG, literal);
}

void CloseDiskLog() {
  Lock lock;
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
  g_diskDisabled = false;
  g_openFailed = false;
  g_tornLine = false;
}

bool WriteAll(int fd, const char* data, std::size_t size, int* err, WriteFn write, std::size_t* written) {
  std::size_t done = 0;
  bool ok = true;
  while (done < size) {
    const ssize_t n = write(fd, data + done, size - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      *err = errno;
      ok = false;
      break;
    }
    if (n == 0) {
      *err = EIO;
      ok = false;
      break;
    }
    done += static_cast<std::size_t>(n);
  }
  if (written != nullptr) *written = done;
  return ok;
}

bool WriteRecord(int fd, const std::string& line, bool* torn, int* err, WriteFn write) {
  const std::size_t prefix = *torn ? 1 : 0;
  const std::string out = *torn ? "\n" + line : line;
  std::size_t written = 0;
  if (WriteAll(fd, out.data(), out.size(), err, write, &written)) {
    *torn = false;
    return true;
  }
  if (written > prefix) {
    *torn = true;  // part of this record is in the file
  } else if (prefix == 1 && written == 1) {
    *torn = false;  // the separating newline went out; nothing of this record did
  }
  return false;
}

void Emit(nevr_quest::LogLevel level, const std::string& message) {
  __android_log_write(Priority(level), NEVR_TAG, message.c_str());

  Lock lock;
  if (g_diskDisabled) return;
  if (g_fd < 0) {
    if (g_openFailed) {
      const long long now = NowMs();
      if (now >= g_lastOpenFailMs && now - g_lastOpenFailMs < kOpenRetryMs) return;
    }
    if (!OpenDiskLog()) return;
  }
  const std::string line = nevr_quest::FormatDiskLogLine(level, NowMs(), message);
  int err = 0;
  if (!WriteRecord(g_fd, line, &g_tornLine, &err, ::write)) {
    ReportDiskFailure("write failed", err, &g_writeFailureReported);
    return;
  }
  g_logBytes += static_cast<long long>(line.size());
  if (g_logBytes >= kMaxDiskLogBytes) {
    // Close so the next record opens, rotates and starts a fresh file.
    ::close(g_fd);
    g_fd = -1;
  }
}

}  // namespace sentinel

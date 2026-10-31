#include "sentinel_log.h"

#include "core/crash_report.h"
#include "quest/sentinel/data_root.h"

#include <android/log.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

// A build that names the folder (the host tests) logs there and nowhere else; the shipped build has no
// definition and gets the two real candidates in LogDir().
#ifdef NEVR_QUEST_FILES_DIR
#define NEVR_QUEST_LOG_DIR_OVERRIDE 1
#else
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
bool g_diskDisabled = false;     // the path opened but is not a regular file: never retried
bool g_openFailed = false;       // the last open failed; retried after kOpenRetryMs
long long g_lastOpenFailMs = 0;
long long g_logBytes = 0;        // size of the open log: its size at open plus what this run wrote
bool g_rotateFailed = false;     // the last rename failed; retried after kOpenRetryMs
// The chosen log folder. Fixed buffers, not std::string: a namespace-scope std::string has a dynamic
// initializer and an atexit destructor, and the ELF constructor logs before either could be set up.
constexpr std::size_t kMaxDirBytes = 400;
constexpr char kSessionPrefix[] = "nevr-sentinel.session-";
char g_logDir[kMaxDirBytes + 1] = "";
bool g_dirResolved = false;
bool g_sessionKept = false;      // this process has already dealt with the log a previous process left
// Lines about choosing the folder wait here for the disk: they are made while the open is in progress (g_mutex
// held), and the file they belong in is not open until it has succeeded. Logcat has them at once.
struct PendingLine {
  nevr_quest::LogLevel level;
  char text[768];
};
constexpr std::size_t kMaxPending = 6;
PendingLine g_pending[kMaxPending] = {};
std::size_t g_pendingHead = 0;   // the next line to write
std::size_t g_pendingCount = 0;  // lines queued since the queue last emptied
// The errno last reported for each candidate, so a candidate that keeps failing the same way is reported once
// and not at every retry; a different errno is reported again.
constexpr std::size_t kMaxCandidates = 8;
int g_reportedErrno[kMaxCandidates] = {};
const LogDirCandidate* g_candidates = nullptr;  // null: the built-in list
std::size_t g_candidateCount = 0;
bool g_rotationFailureReported = false;
long long g_lastRotateFailMs = 0;
unsigned g_openAttempts = 0;
NowFn g_now = nullptr;           // test clock for both clocks
NowFn g_wallNow = nullptr;       // test wall clock only
RenameFn g_rename = nullptr;

class Lock {
 public:
  Lock() { pthread_mutex_lock(&g_mutex); }
  ~Lock() { pthread_mutex_unlock(&g_mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

const char* LevelName(nevr_quest::LogLevel level) {
  switch (level) {
    case nevr_quest::LogLevel::kInfo: return "INFO";
    case nevr_quest::LogLevel::kWarn: return "WARN";
    case nevr_quest::LogLevel::kError: break;
  }
  return "ERROR";
}

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

// Wall clock: record timestamps and rotated names only.
long long NowMs() {
  if (g_wallNow != nullptr) return g_wallNow();
  if (g_now != nullptr) return g_now();
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000L;
}

// Every retry interval is measured here: a wall-clock step (NTP, a user setting the time) must not
// move a throttle window.
long long MonotonicMs() {
  if (g_now != nullptr) return g_now();
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000L;
}

// Android/media/<package> is made by the platform for the app and holds the game's data, so only the
// folders below it are created; Android/data is created from its top.
constexpr LogDirCandidate kShippedCandidates[] = {
    {"android_media", nevr_quest::data_root::kMediaRoot, "/sdcard/Android/media/com.readyatdawn.r15"},
    {"android_data", "/sdcard/Android/data/com.readyatdawn.r15/files", "/sdcard/Android/data"},
};
#ifdef NEVR_QUEST_LOG_DIR_OVERRIDE
constexpr LogDirCandidate kBuiltInCandidates[] = {
    {"override", NEVR_QUEST_FILES_DIR, NEVR_QUEST_FILES_DIR},
};
#else
constexpr const LogDirCandidate (&kBuiltInCandidates)[2] = kShippedCandidates;
#endif

const LogDirCandidate* Candidates(std::size_t* count) {
  if (g_candidates != nullptr) {
    *count = g_candidateCount;
    return g_candidates;
  }
  *count = std::size(kBuiltInCandidates);
  return kBuiltInCandidates;
}

// True while a failed rename is inside its retry window.
bool RotationBlocked() {
  if (!g_rotateFailed) return false;
  const long long now = MonotonicMs();
  return now >= g_lastRotateFailMs && now - g_lastRotateFailMs < kOpenRetryMs;
}

// A failed rotation is logged once until one succeeds, and not attempted again for kOpenRetryMs.
void RotationFailed(const char* what, int err) {
  g_rotateFailed = true;
  g_lastRotateFailMs = MonotonicMs();
  if (g_rotationFailureReported) return;
  g_rotationFailureReported = true;
  __android_log_print(ANDROID_LOG_WARN, NEVR_TAG, "on-disk log rotation %s errno=%d (%s)", what, err,
                      std::strerror(err));
}

// Renames a log that has reached the size bound to a name that does not exist yet; nothing is
// ever deleted or replaced. Best effort: on failure the log keeps growing, the failure is
// logged once, and the next attempt is a retry interval later.
void RotateIfLarge(const std::string& dir, const std::string& path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < kMaxDiskLogBytes) return;
  if (RotationBlocked()) return;
  const std::string base = dir + "/nevr-sentinel." + std::to_string(NowMs());
  for (int n = 0; n < 1000; ++n) {
    const std::string rotated = base + (n == 0 ? "" : "." + std::to_string(n)) + ".log";
    struct stat taken {};
    if (::lstat(rotated.c_str(), &taken) == 0) continue;
    if ((g_rename != nullptr ? g_rename : &::rename)(path.c_str(), rotated.c_str()) != 0) {
      RotationFailed("failed", errno);
    } else {
      g_rotateFailed = false;
      g_rotationFailureReported = false;
    }
    return;
  }
  RotationFailed("skipped: no free name", 0);
}

void FailOpen(int err, const char* what) {
  g_openFailed = true;
  g_lastOpenFailMs = MonotonicMs();
  ReportDiskFailure(what, err, &g_openFailureReported);
}

void QueueLine(nevr_quest::LogLevel level, const nlohmann::json& line);

// The first open of a process keeps the log a previous process left: it is renamed, whole, to
// `nevr-sentinel.session-<unix ms>.log` (a name that does not exist yet; nothing is ever replaced or deleted), so
// the new session starts a file of its own and the one before it is still readable from its first line. An absent or
// empty log leaves nothing to keep. A rename that fails leaves the old log where it is, appended to as before, and is
// told once; it is not retried in this process.
void KeepPreviousSession(const std::string& dir, const std::string& path) {
  if (g_sessionKept) return;
  g_sessionKept = true;
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) return;
  const std::string base = dir + "/" + kSessionPrefix + std::to_string(NowMs());
  for (int n = 0; n < 1000; ++n) {
    const std::string kept = base + (n == 0 ? "" : "." + std::to_string(n)) + ".log";
    struct stat taken {};
    if (::lstat(kept.c_str(), &taken) == 0) continue;
    nlohmann::json line = nlohmann::json::object();
    line["kept_as"] = kept;
    line["bytes"] = static_cast<long long>(st.st_size);
    if ((g_rename != nullptr ? g_rename : &::rename)(path.c_str(), kept.c_str()) != 0) {
      line["event"] = "sentinel_log_session_keep_failed";
      line["errno"] = errno;
      line["error"] = std::strerror(errno);
      line["msg"] = "the previous session's log stays in nevr-sentinel.log and this session appends to it";
      QueueLine(nevr_quest::LogLevel::kWarn, line);
    } else {
      line["event"] = "sentinel_log_session_kept";
      line["msg"] = "the previous session's log is kept whole in kept_as; this session starts nevr-sentinel.log";
      QueueLine(nevr_quest::LogLevel::kInfo, line);
    }
    return;
  }
}

// Rotates a log at the size bound, then opens `dir`/nevr-sentinel.log. The open is non-blocking and
// never follows a symlink.
int OpenLogFile(const std::string& dir) {
  const std::string path = dir + "/nevr-sentinel.log";
  KeepPreviousSession(dir, path);
  RotateIfLarge(dir, path);
  return ::open(path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, 0644);
}

// One line about the log itself: logcat now, the disk when the log is open (the pending queue).
void QueueLine(nevr_quest::LogLevel level, const nlohmann::json& line) {
  const std::string text = line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  __android_log_write(Priority(level), NEVR_TAG, text.c_str());
  if (g_pendingHead == g_pendingCount) g_pendingHead = g_pendingCount = 0;
  if (g_pendingCount < kMaxPending && text.size() < sizeof(g_pending[0].text)) {
    g_pending[g_pendingCount].level = level;
    std::memcpy(g_pending[g_pendingCount].text, text.c_str(), text.size() + 1);
    ++g_pendingCount;
  }
}

// One line about choosing the folder. Every line names the candidate and its folder; a failure adds the errno
// and its text.
void ReportChoice(nevr_quest::LogLevel level, const char* event, const char* name, const std::string& dir,
                  int err, const char* msg) {
  nlohmann::json line = nlohmann::json::object();
  line["event"] = event;
  line["candidate"] = name;
  line["dir"] = dir;
  if (err != 0) {
    line["errno"] = err;
    line["error"] = std::strerror(err);
  }
  line["msg"] = msg;
  QueueLine(level, line);
}

// g_mutex held. Opens the log in the chosen folder; with none chosen yet, takes the first candidate whose
// folder can be made and whose log file opens, and remembers it.
int OpenInChosenDir(int* err) {
  if (g_dirResolved) {
    const int fd = OpenLogFile(g_logDir);
    if (fd < 0) {
      *err = errno;
      // The folder itself vanished (the user cleared the app's files): choose again at the next try.
      if (*err == ENOENT || *err == ENOTDIR) g_dirResolved = false;
    }
    return fd;
  }
  std::size_t count = 0;
  const LogDirCandidate* list = Candidates(&count);
  for (std::size_t i = 0; i < count; ++i) {
    if (EnsureDirBelow(list[i])) {
      const int fd = OpenLogFile(list[i].dir);
      if (fd >= 0) {
        std::strncpy(g_logDir, list[i].dir, kMaxDirBytes);
        g_dirResolved = true;
        for (int& reported : g_reportedErrno) reported = 0;  // a later failure of any candidate is news again
        ReportChoice(nevr_quest::LogLevel::kInfo, "sentinel_log_dir", list[i].name, list[i].dir, 0,
                     "nevr-sentinel.log and its rotated files are written in this folder");
        return fd;
      }
    }
    *err = errno;
    if (i < kMaxCandidates && g_reportedErrno[i] != *err) {
      g_reportedErrno[i] = *err;
      ReportChoice(nevr_quest::LogLevel::kWarn, "sentinel_log_dir_failed", list[i].name, list[i].dir, *err,
                   "this folder cannot hold nevr-sentinel.log; the next candidate is tried");
    }
  }
  return -1;
}

// g_mutex held. The target must be a regular file: a FIFO or device at the log path must not stall the
// ELF constructor.
bool OpenDiskLog() {
  ++g_openAttempts;
  int openErr = 0;
  const int fd = OpenInChosenDir(&openErr);
  if (fd < 0) {
    FailOpen(openErr, "open failed");
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

// g_mutex held, g_fd open. One record to the file; at the size bound closes it so the next record opens,
// rotates and starts a fresh file. While a failed rotation is inside its retry window the file stays
// open: reopening every record would only fail again.
bool AppendRecord(nevr_quest::LogLevel level, const std::string& message) {
  const std::string line = FormatDiskLogLine(level, NowMs(), message);
  int err = 0;
  if (!WriteRecord(g_fd, line, &g_tornLine, &err, ::write)) {
    ReportDiskFailure("write failed", err, &g_writeFailureReported);
    return false;
  }
  g_logBytes += static_cast<long long>(line.size());
  if (g_logBytes >= kMaxDiskLogBytes && !RotationBlocked()) {
    ::close(g_fd);
    g_fd = -1;
  }
  return true;
}

}  // namespace

// mkdir -p of `c.dir` below `c.trusted`, one folder at a time: each must be a directory by lstat, so a
// link planted below the trusted part is refused rather than followed. True when every folder below the
// trusted part is there; whether the process may write into it is the log file's open. On false errno says why.
bool EnsureDirBelow(const LogDirCandidate& c) {
  const std::size_t base = std::strlen(c.trusted);
  const std::size_t len = std::strlen(c.dir);
  if (len > kMaxDirBytes || std::strncmp(c.dir, c.trusted, base) != 0 ||
      (len > base && c.dir[base] != '/')) {
    errno = EINVAL;
    return false;
  }
  const std::string dir(c.dir, len);
  std::size_t pos = base;
  while (pos < len) {
    std::size_t next = dir.find('/', pos + 1);
    if (next == std::string::npos) next = len;
    const std::string part = dir.substr(0, next);
    if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    struct stat st {};
    if (::lstat(part.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return false;
    }
    pos = next;
  }
  return true;  // the log file's own open, next, is the check that the folder takes a write
}

const char* FilesDir() { return NEVR_QUEST_FILES_DIR; }

// "<digits>" or "<digits>.<digits>" between the prefix and ".log": the session's unix ms and its collision number.
static bool ParseSessionName(const char* name, unsigned long long* ms, unsigned long long* n) {
  const std::size_t prefix = sizeof(kSessionPrefix) - 1;
  const std::size_t len = std::strlen(name);
  if (len <= prefix + 4 || std::strncmp(name, kSessionPrefix, prefix) != 0 || std::strcmp(name + len - 4, ".log") != 0) {
    return false;
  }
  const std::string middle(name + prefix, len - prefix - 4);
  const std::size_t dot = middle.find('.');
  const std::string first = middle.substr(0, dot);
  const std::string second = dot == std::string::npos ? "0" : middle.substr(dot + 1);
  for (const std::string* part : {&first, &second}) {
    if (part->empty()) return false;
    for (const char c : *part) {
      if (c < '0' || c > '9') return false;
    }
  }
  *ms = std::strtoull(first.c_str(), nullptr, 10);
  *n = std::strtoull(second.c_str(), nullptr, 10);
  return true;
}

std::string PreviousSessionLog() {
  const std::string dir = LogDir();
  DIR* handle = ::opendir(dir.c_str());
  if (handle == nullptr) return "";
  std::string best;
  unsigned long long bestMs = 0;
  unsigned long long bestN = 0;
  while (const dirent* entry = ::readdir(handle)) {
    unsigned long long ms = 0;
    unsigned long long n = 0;
    if (!ParseSessionName(entry->d_name, &ms, &n)) continue;
    if (best.empty() || ms > bestMs || (ms == bestMs && n > bestN)) {
      best = entry->d_name;
      bestMs = ms;
      bestN = n;
    }
  }
  ::closedir(handle);
  return best.empty() ? best : dir + "/" + best;
}

void ResetSessionForTest() {
  Lock lock;
  g_sessionKept = false;
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
}

std::string LogDir() {
  Lock lock;
  if (g_dirResolved) return g_logDir;
  std::size_t count = 0;
  const LogDirCandidate* list = Candidates(&count);
  return count > 0 ? list[0].dir : "";
}

const LogDirCandidate* ShippedLogDirCandidatesForTest(std::size_t* count) {
  *count = std::size(kShippedCandidates);
  return kShippedCandidates;
}

void SetLogDirCandidatesForTest(const LogDirCandidate* list, std::size_t count) {
  Lock lock;
  g_candidates = (list != nullptr && count > 0) ? list : nullptr;
  g_candidateCount = g_candidates != nullptr ? count : 0;
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
  g_dirResolved = false;
  g_pendingHead = g_pendingCount = 0;
  for (int& reported : g_reportedErrno) reported = 0;
  g_openFailed = false;
  g_diskDisabled = false;
}

void SetClockForTest(NowFn now) { g_now = now; }
void SetWallClockForTest(NowFn now) { g_wallNow = now; }
void SetRenameForTest(RenameFn rename) { g_rename = rename; }
unsigned OpenAttemptsForTest() { return g_openAttempts; }

void EmitFixed(nevr_quest::LogLevel level, const char* literal) noexcept {
  nevr_crash::RecordLogLine(literal, std::strlen(literal));
  __android_log_write(Priority(level), NEVR_TAG, literal);
}

void CloseDiskLog() {
  Lock lock;
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
  g_diskDisabled = false;
  g_openFailed = false;
  g_rotateFailed = false;
  g_rotationFailureReported = false;
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

std::string FormatDiskLogLine(nevr_quest::LogLevel level, long long unixMs, const std::string& message) {
  // sax_parse into a DOM, not json::parse: parse() instantiates the callback parser, whose std::function drags
  // std::bad_function_call's typeinfo into the .so as a default-visibility export (TestExportAllowlist).
  nlohmann::json line;
  nlohmann::detail::json_sax_dom_parser<nlohmann::json,
                                        nlohmann::detail::iterator_input_adapter<std::string::const_iterator>>
      dom(line, false);
  if (!nlohmann::json::sax_parse(message, &dom)) line = nlohmann::json();
  if (line.is_object()) {
    line.erase("ts_ms");
    if (!line.contains("level")) line["level"] = LevelName(level);
  } else {
    line = nlohmann::json::object();
    line["level"] = LevelName(level);
    line["msg"] = message;
  }
  line["ts_unix_ms"] = unixMs;
  line["tag"] = NEVR_TAG;
  return line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

void Emit(nevr_quest::LogLevel level, const std::string& message) {
  // The crash report (#1605) prints the last 50 sentinel lines; the ring is lock-free and allocation-free.
  nevr_crash::RecordLogLine(message.data(), message.size());
  __android_log_write(Priority(level), NEVR_TAG, message.c_str());

  Lock lock;
  if (g_diskDisabled) return;
  if (g_fd < 0) {
    if (g_openFailed) {
      const long long now = MonotonicMs();
      if (now >= g_lastOpenFailMs && now - g_lastOpenFailMs < kOpenRetryMs) return;
    }
    if (!OpenDiskLog()) return;
  }
  // The lines about choosing the folder go first, in order; one that fails to write stays queued for the next record.
  while (g_pendingHead < g_pendingCount) {
    if (g_fd < 0 && !OpenDiskLog()) return;  // a line filled the log to its bound and closed it
    if (!AppendRecord(g_pending[g_pendingHead].level, g_pending[g_pendingHead].text)) return;
    ++g_pendingHead;
  }
  if (g_fd < 0 && !OpenDiskLog()) return;
  AppendRecord(level, message);
}

}  // namespace sentinel

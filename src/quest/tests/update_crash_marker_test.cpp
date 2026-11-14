// Host test for the self-update's crash marker (src/quest/sentinel/crash_report_quest.cpp: ArmUpdateCrashMarker and
// MarkUpdateCrashed; docs/adr/0024-pc-self-update.md, section 6). A crash while update/pending.json exists leaves the empty
// update/crashed and one `crash_marker` record in the run's log (sentinel_log.h); with no pending.json it leaves neither.
//
// The fork tests crash for real: the child arms the marker, raises SIGSEGV, and a handler marks and then lets the signal end
// the child, as the sentinel's report action does. The parent reads update/crashed, the log file the child wrote and the child's
// wait status. The other tests call the marker in process and read its outcome, the log file and the lines __android_log_write is
// asked to write (the crash path writes none; arming writes one only when the record is unavailable).
//
// The sentinel log is pointed at `<root>/nevr/logs` through SetLogDirCandidatesForTest, below the test's root, so every test reads
// the file the marker's descriptor and the log writer share. The record is parsed with nlohmann::json: the marker builds its line
// by hand, because it runs in a signal handler where nothing may allocate.
//
// This translation unit includes crash_report_quest.cpp itself, as crash_report_quest_test.cpp does, so the device build's
// helpers that the host build leaves unused are allowed here.

#include <android/log.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/update/names.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#include "quest/sentinel/crash_report_quest.cpp"  // NOLINT: the marker and the report's file in one unit, as the report's test does
#pragma GCC diagnostic warning "-Wunused-function"

namespace {

// What the marker asked logcat to write (arming writes the record-unavailable Warning, the crash path nothing: the arm's other
// lines go to __android_log_print below).
struct Line {
  int priority;
  std::string text;
};
std::vector<Line> g_lines;
int g_fail = 0;

constexpr int kChildSurvived = 3;      // the child's exit status when the raise did not end it (the test then fails)
constexpr mode_t kMarkerMode = 0600;   // the mode the marker creates update/crashed with
constexpr mode_t kTestFileMode = 0600;
constexpr mode_t kTestDirMode = 0700;
constexpr mode_t kModeBits = 0777;     // the permission bits of st_mode
constexpr char kTempTemplate[] = "nevr-crash-marker-XXXXXX";
constexpr char kOddRootName[] = "q\"b\\c\td";  // a folder name with a quote, a backslash and a tab: each is escaped in the record's path

constexpr std::size_t kWrittenKeyCount = 8;  // event, level, outcome, path, run, tag, thread, ts
constexpr std::size_t kFailedKeyCount = 9;   // the same and errno
constexpr std::size_t kStampLength = 24;     // YYYY-MM-DDTHH:MM:SS.mmmZ
constexpr int kStampFields = 7;              // year, month, day, hour, minute, second, milliseconds
constexpr int kTmYearBase = 1900;
constexpr long kStampToleranceSeconds = 120;  // how far the record's time may be from the test's clock
constexpr int kMillisPerSecond = 1000;
constexpr std::size_t kLineBytes = 2048;  // room for any record the tests format
constexpr std::size_t kTinyBytes = 16;    // too small for any record
constexpr std::uint64_t kFakeThread = 4242;

// Instants whose UTC date the record's `ts` must show, and the date as text. The leap days, the end of a year, a century that is not
// a leap year and a clock before the epoch each take a different branch of the calendar arithmetic.
constexpr std::time_t kLeapDay2000Seconds = 951782400;
constexpr std::time_t kLeapDay2024Seconds = 1709164800;
constexpr std::time_t kNovember2023Seconds = 1700000000;
constexpr long kNovember2023Nanos = 123456789;
constexpr std::time_t kLastSecond2024Seconds = 1735689599;
constexpr long kLastNanos = 999999999;
constexpr std::time_t kCentury2100Seconds = 4102444800;
constexpr std::time_t kBeforeEpochSeconds = -1;

struct StampCase {
  std::time_t seconds;
  long nanos;
  const char* expected;
};
const StampCase kStampCases[] = {
    {0, 0, "1970-01-01T00:00:00.000Z"},
    {kLeapDay2000Seconds, 0, "2000-02-29T00:00:00.000Z"},
    {kLeapDay2024Seconds, 0, "2024-02-29T00:00:00.000Z"},
    {kNovember2023Seconds, kNovember2023Nanos, "2023-11-14T22:13:20.123Z"},
    {kLastSecond2024Seconds, kLastNanos, "2024-12-31T23:59:59.999Z"},
    {kCentury2100Seconds, 0, "2100-01-01T00:00:00.000Z"},
    {kBeforeEpochSeconds, 0, "1970-01-01T00:00:00.000Z"},
};

void Check(bool ok, const std::string& what) {
  if (ok) return;
  ++g_fail;
  std::fprintf(stderr, "update_crash_marker_test: FAIL %s\n", what.c_str());
}

bool EntryExists(const std::string& path) {
  struct stat st;
  std::memset(&st, 0, sizeof(st));
  return ::lstat(path.c_str(), &st) == 0;
}

bool WriteFile(const std::string& path, const char* text) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kTestFileMode);
  if (fd < 0) return false;
  const std::size_t size = std::strlen(text);
  const bool wrote = ::write(fd, text, size) == static_cast<ssize_t>(size);
  ::close(fd);
  return wrote;
}

// Removes what the tests make under a root: a file, a link or an empty folder at each known place, then the root.
void RemoveEntry(const std::string& path) {
  if (::unlink(path.c_str()) != 0) ::rmdir(path.c_str());
}

// The log files under <root>/nevr/logs, then the two folders (or the file a test put at <root>/nevr).
void RemoveLogFolder(const std::string& root) {
  const std::string logs = root + "/nevr/logs";
  if (DIR* handle = ::opendir(logs.c_str())) {
    while (const dirent* entry = ::readdir(handle)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") ::unlink((logs + "/" + name).c_str());
    }
    ::closedir(handle);
  }
  RemoveEntry(logs);
  RemoveEntry(root + "/nevr");
}

void RemoveRoot(const std::string& root) {
  RemoveEntry(root + "/update/crashed");
  RemoveEntry(root + "/update/pending.json");
  RemoveEntry(root + "/elsewhere");
  RemoveEntry(root + "/update");
  RemoveLogFolder(root);
  RemoveEntry(root);
}

// A new root with an update/ folder in it, made under TMPDIR (or the current folder). Empty when it cannot be made.
std::string MakeRoot() {
  const char* base = std::getenv("TMPDIR");
  const std::string pattern = std::string(base != nullptr && base[0] != '\0' ? base : ".") + "/" + kTempTemplate;
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (::mkdtemp(buffer.data()) == nullptr) return std::string();
  const std::string root(buffer.data());
  if (::mkdir((root + "/update").c_str(), kTestDirMode) != 0) {
    RemoveRoot(root);
    return std::string();
  }
  return root;
}

// The sentinel log's folder for the test: <root>/nevr/logs, created below <root> and never above it. The strings outlive the
// candidate list, as SetLogDirCandidatesForTest asks. The process's log name is forgotten, so the next arm makes this test's own.
std::string g_logTrusted;
std::string g_logDir;
sentinel::LogDirCandidate g_candidate;

void UseLogFolder(const std::string& root) {
  g_logTrusted = root;
  g_logDir = root + "/nevr/logs";
  g_candidate = sentinel::LogDirCandidate{"test", g_logDir.c_str(), g_logTrusted.c_str()};
  sentinel::SetLogDirCandidatesForTest(&g_candidate, 1);
  sentinel::ResetSessionForTest();
}

// A new root whose log folder is the sentinel log's. Empty when the root cannot be made.
std::string MakeRootWithLog() {
  const std::string root = MakeRoot();
  if (!root.empty()) UseLogFolder(root);
  return root;
}

// Every parsed line of every `nevr-*.jsonl` under <root>/nevr/logs, files in name order, lines in file order.
struct LogRead {
  std::size_t files = 0;
  std::size_t unparsable = 0;
  std::vector<nlohmann::json> records;
};

bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

LogRead ReadLog(const std::string& root) {
  LogRead log;
  const std::string logs = root + "/nevr/logs";
  std::vector<std::string> names;
  if (DIR* handle = ::opendir(logs.c_str())) {
    while (const dirent* entry = ::readdir(handle)) {
      const std::string name = entry->d_name;
      if (name.rfind("nevr-", 0) == 0 && EndsWith(name, ".jsonl")) names.push_back(name);
    }
    ::closedir(handle);
  }
  std::sort(names.begin(), names.end());
  log.files = names.size();
  for (const std::string& name : names) {
    std::ifstream in(logs + "/" + name);
    std::string text;
    while (std::getline(in, text)) {
      if (text.empty()) continue;
      nlohmann::json record = nlohmann::json::parse(text, nullptr, false);
      if (record.is_discarded()) {
        ++log.unparsable;
      } else {
        log.records.push_back(record);
      }
    }
  }
  return log;
}

bool IsString(const nlohmann::json& record, const char* key, const std::string& value) {
  return record.is_object() && record.contains(key) && record[key].is_string() && record[key].get<std::string>() == value;
}

bool IsInteger(const nlohmann::json& record, const char* key, long long value) {
  return record.is_object() && record.contains(key) && record[key].is_number_integer() && record[key].get<long long>() == value;
}

std::vector<nlohmann::json> Markers(const LogRead& log) {
  std::vector<nlohmann::json> markers;
  for (const nlohmann::json& record : log.records) {
    if (IsString(record, "event", "crash_marker")) markers.push_back(record);
  }
  return markers;
}

// `YYYY-MM-DDTHH:MM:SS.mmmZ`, within a couple of minutes of now. timegm is libc's calendar; the record's is the marker's own.
bool StampIsNow(const std::string& stamp) {
  if (stamp.size() != kStampLength) return false;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millis = 0;
  if (std::sscanf(stamp.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &year, &month, &day, &hour, &minute, &second, &millis) != kStampFields) {
    return false;
  }
  struct tm utc {};
  utc.tm_year = year - kTmYearBase;
  utc.tm_mon = month - 1;
  utc.tm_mday = day;
  utc.tm_hour = hour;
  utc.tm_min = minute;
  utc.tm_sec = second;
  const long delta = static_cast<long>(::timegm(&utc)) - static_cast<long>(std::time(nullptr));
  return (delta < 0 ? -delta : delta) <= kStampToleranceSeconds && millis >= 0 && millis < kMillisPerSecond && stamp.back() == 'Z';
}

// One record, field by field: the PC record's `outcome`, `path` and `thread`, the log's header keys, `errno` for a failed marker
// only, and no other key.
void CheckRecord(const nlohmann::json& record, bool written, int failure, const std::string& crashedPath, long long thread,
                 const std::string& runPrefix, const std::string& scope) {
  Check(record.is_object(), scope + ": the record is a JSON object");
  if (!record.is_object()) return;
  Check(IsString(record, "event", "crash_marker"), scope + ": event is crash_marker");
  Check(IsString(record, "tag", "NEVR.HOST"), scope + ": tag is NEVR.HOST");
  Check(IsString(record, "outcome", written ? "written" : "failed"), scope + ": the outcome is named");
  Check(IsString(record, "level", written ? "info" : "warn"), scope + ": Info for written, Warning for failed");
  Check(IsString(record, "path", crashedPath), scope + ": the path is the crashed file, escaped and parsed back whole");
  Check(IsInteger(record, "thread", thread), scope + ": the thread is the faulting thread's id");
  Check(record.contains("run") && record["run"].is_string() && record["run"].get<std::string>().rfind(runPrefix, 0) == 0,
        scope + ": the run id is this process's");
  Check(record.contains("ts") && record["ts"].is_string() && StampIsNow(record["ts"].get<std::string>()),
        scope + ": ts is a UTC stamp with milliseconds, now");
  if (written) {
    Check(!record.contains("errno"), scope + ": a written marker has no errno");
  } else {
    Check(IsInteger(record, "errno", failure), scope + ": a failed marker names the errno of the open");
  }
  Check(record.size() == (written ? kWrittenKeyCount : kFailedKeyCount), scope + ": no key beyond the record's own");
}

long long ThisThread() { return static_cast<long long>(::syscall(SYS_gettid)); }

std::string HexPrefix(pid_t pid) {
  char text[kLineBytes];
  std::snprintf(text, sizeof(text), "%x-", static_cast<unsigned>(pid));
  return text;
}

// The report action's stand-in: marks the crash, then restores the default action and raises the signal again, so the process
// ends by SIGSEGV when the handler returns.
void MarkThenDie(int, siginfo_t*, void*) {
  sentinel::MarkUpdateCrashed();
  struct sigaction fallback;
  std::memset(&fallback, 0, sizeof(fallback));
  fallback.sa_handler = SIG_DFL;
  ::sigaction(SIGSEGV, &fallback, nullptr);
  ::raise(SIGSEGV);
}

// Forks a child that arms the marker on `root` and raises SIGSEGV. False when the fork or the wait fails; `status` gets the
// child's wait status and `childPid` its id.
bool RunCrashingChild(const std::string& root, int* status, pid_t* childPid) {
  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    sentinel::ArmUpdateCrashMarker(root.c_str());
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = &MarkThenDie;
    action.sa_flags = SA_SIGINFO;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(SIGSEGV, &action, nullptr);
    ::raise(SIGSEGV);
    ::_exit(kChildSurvived);
  }
  *childPid = pid;
  return ::waitpid(pid, status, 0) == pid;
}

void TestCrashWithPendingLeavesTheMarker() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(WriteFile(root + "/update/pending.json", "{}"), "pending.json is written");
  int status = 0;
  pid_t child = 0;
  Check(RunCrashingChild(root, &status, &child), "the child forks and is waited for");
  Check(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the child ended by SIGSEGV");
  Check(EntryExists(root + "/update/crashed"), "a crash with pending.json leaves update/crashed");
  struct stat st;
  std::memset(&st, 0, sizeof(st));
  Check(::stat((root + "/update/crashed").c_str(), &st) == 0 && st.st_size == 0, "update/crashed is empty");
  Check((st.st_mode & kModeBits) == kMarkerMode, "update/crashed is created with mode 0600");

  const LogRead log = ReadLog(root);
  Check(log.files == 1, "the child wrote one log file");
  Check(log.unparsable == 0, "every line of the log is a JSON object");
  const std::vector<nlohmann::json> markers = Markers(log);
  Check(markers.size() == 1, "a crash with pending.json leaves exactly one crash_marker record");
  if (markers.size() == 1) {
    CheckRecord(markers[0], true, 0, root + "/update/crashed", static_cast<long long>(child), HexPrefix(child), "forked crash");
  }
  RemoveRoot(root);
}

void TestCrashWithoutPendingLeavesNothing() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  int status = 0;
  pid_t child = 0;
  Check(RunCrashingChild(root, &status, &child), "the child forks and is waited for");
  Check(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the child ended by SIGSEGV");
  Check(!EntryExists(root + "/update/crashed"), "a crash without pending.json leaves no update/crashed");
  const LogRead log = ReadLog(root);
  Check(log.files == 1 && log.unparsable == 0, "the child opened its log file at arm time and left it parseable");
  Check(Markers(log).empty(), "a crash without pending.json leaves no crash_marker record");
  RemoveRoot(root);
}

void TestNotArmedWritesNothing() {
  Check(!sentinel::ArmUpdateCrashMarker(nullptr), "a null root does not arm");
  Check(!sentinel::ArmUpdateCrashMarker(""), "an empty root does not arm");
  g_lines.clear();
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kNotArmed, "unarmed: the outcome is not_armed");
  Check(g_lines.empty(), "unarmed: nothing to logcat");
}

void TestArmedWithoutPendingWritesNothing() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(sentinel::ArmUpdateCrashMarker(root.c_str()), "the root arms the marker");
  g_lines.clear();
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kNoPending, "no pending.json: the outcome is no_pending");
  Check(!EntryExists(root + "/update/crashed"), "no pending.json: no marker file");
  Check(g_lines.empty(), "no pending.json: nothing to logcat");
  Check(Markers(ReadLog(root)).empty(), "no pending.json: no crash_marker record");
  RemoveRoot(root);
}

void TestWrittenMarkerAndItsRecord() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(WriteFile(root + "/update/pending.json", "{}"), "pending.json is written");
  Check(sentinel::ArmUpdateCrashMarker(root.c_str()), "the root arms the marker");
  g_lines.clear();
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kWritten, "pending.json present: the outcome is written");
  Check(EntryExists(root + "/update/crashed"), "pending.json present: update/crashed is there");
  Check(g_lines.empty(), "written: the crash path writes nothing to logcat");
  const LogRead log = ReadLog(root);
  Check(log.files == 1 && log.unparsable == 0, "written: one log file, every line parses");
  const std::vector<nlohmann::json> markers = Markers(log);
  Check(markers.size() == 1, "written: exactly one record");
  if (markers.size() == 1) {
    CheckRecord(markers[0], true, 0, root + "/update/crashed", ThisThread(), sentinel::LogRunId(), "written");
  }
  RemoveRoot(root);
}

void TestRecordSharesTheLogFile() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(WriteFile(root + "/update/pending.json", "{}"), "pending.json is written");
  Check(sentinel::ArmUpdateCrashMarker(root.c_str()), "the root arms the marker");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "before the crash");
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kWritten, "the marker is written between two log records");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "after the crash");
  sentinel::CloseDiskLog();
  const LogRead log = ReadLog(root);
  Check(log.files == 1, "the log writer and the marker's descriptor share one file");
  Check(log.unparsable == 0, "the log's records and the marker's record are all whole lines");
  std::size_t before = log.records.size();
  std::size_t marker = log.records.size();
  std::size_t after = log.records.size();
  for (std::size_t i = 0; i < log.records.size(); ++i) {
    if (IsString(log.records[i], "msg", "before the crash")) before = i;
    if (IsString(log.records[i], "event", "crash_marker")) marker = i;
    if (IsString(log.records[i], "msg", "after the crash")) after = i;
  }
  Check(before < marker && marker < after && after < log.records.size(), "the record sits between the two records, in write order");
  if (marker < log.records.size() && before < log.records.size()) {
    Check(log.records[marker]["run"] == log.records[before]["run"], "the record carries the run id the log writer stamps");
  }
  RemoveRoot(root);
}

void TestFailedMarkerNamesErrno() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(WriteFile(root + "/update/pending.json", "{}"), "pending.json is written");
  Check(::mkdir((root + "/update/crashed").c_str(), kTestDirMode) == 0, "a folder sits where the marker goes");
  Check(sentinel::ArmUpdateCrashMarker(root.c_str()), "the root arms the marker");
  g_lines.clear();
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kFailed, "a folder in the marker's place: the outcome is failed");
  Check(g_lines.empty(), "failed: the crash path writes nothing to logcat");
  const std::vector<nlohmann::json> markers = Markers(ReadLog(root));
  Check(markers.size() == 1, "failed: exactly one record");
  if (markers.size() == 1) {
    CheckRecord(markers[0], false, EISDIR, root + "/update/crashed", ThisThread(), sentinel::LogRunId(), "failed");
  }
  RemoveRoot(root);
}

void TestSymlinkMarkerIsRefused() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(WriteFile(root + "/update/pending.json", "{}"), "pending.json is written");
  Check(::symlink((root + "/elsewhere").c_str(), (root + "/update/crashed").c_str()) == 0, "a link sits where the marker goes");
  Check(sentinel::ArmUpdateCrashMarker(root.c_str()), "the root arms the marker");
  g_lines.clear();
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kFailed, "a link in the marker's place: the outcome is failed");
  Check(!EntryExists(root + "/elsewhere"), "a link in the marker's place is not followed: its target is not created");
  const std::vector<nlohmann::json> markers = Markers(ReadLog(root));
  Check(markers.size() == 1, "a link in the marker's place: exactly one record");
  if (markers.size() == 1) {
    CheckRecord(markers[0], false, ELOOP, root + "/update/crashed", ThisThread(), sentinel::LogRunId(), "link");
  }
  RemoveRoot(root);
}

void TestRootTooLongDisarms() {
  const std::string root(sentinel::kReportPathBytes, 'a');
  Check(!sentinel::ArmUpdateCrashMarker(root.c_str()), "a root too long for the marker's paths does not arm");
  g_lines.clear();
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kNotArmed, "a root too long: the marker stays unarmed");
  Check(g_lines.empty(), "a root too long: nothing to logcat");
}

void TestTrailingSeparatorRoot() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(WriteFile(root + "/update/pending.json", "{}"), "pending.json is written");
  Check(sentinel::ArmUpdateCrashMarker((root + "/").c_str()), "a root ending in a separator arms the marker");
  g_lines.clear();
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kWritten, "a root ending in a separator: the outcome is written");
  Check(EntryExists(root + "/update/crashed"), "a root ending in a separator: the marker is at the joined path");
  const std::vector<nlohmann::json> markers = Markers(ReadLog(root));
  Check(markers.size() == 1, "a root ending in a separator: exactly one record");
  if (markers.size() == 1) {
    CheckRecord(markers[0], true, 0, root + "/update/crashed", ThisThread(), sentinel::LogRunId(),
                "a root ending in a separator: the record names the path with one separator");
  }
  RemoveRoot(root);
}

// A quote, a backslash and a tab in the root's name reach the record's `path` escaped, and the line still parses back to the path.
void TestPathWithSpecialCharactersIsEscaped() {
  const std::string outer = MakeRoot();
  Check(!outer.empty(), "a root can be made");
  if (outer.empty()) return;
  const std::string root = outer + "/" + kOddRootName;
  Check(::mkdir(root.c_str(), kTestDirMode) == 0 && ::mkdir((root + "/update").c_str(), kTestDirMode) == 0,
        "a root with a quote, a backslash and a tab in its name can be made");
  UseLogFolder(root);
  Check(WriteFile(root + "/update/pending.json", "{}"), "pending.json is written");
  Check(sentinel::ArmUpdateCrashMarker(root.c_str()), "the root arms the marker");
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kWritten, "special characters in the path: the outcome is written");
  const LogRead log = ReadLog(root);
  Check(log.unparsable == 0, "special characters in the path: every line parses");
  const std::vector<nlohmann::json> markers = Markers(log);
  Check(markers.size() == 1, "special characters in the path: exactly one record");
  if (markers.size() == 1) {
    CheckRecord(markers[0], true, 0, root + "/update/crashed", ThisThread(), sentinel::LogRunId(), "special characters");
  }
  RemoveRoot(root);
  RemoveRoot(outer);
}

// A log folder that cannot be made leaves the marker armed and working, says so once at arm time, and writes no record.
void TestUnavailableLogLeavesTheMarkerWorking() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(WriteFile(root + "/nevr", "a file where the log's folder goes"), "a file sits where the log's folder goes");
  Check(WriteFile(root + "/update/pending.json", "{}"), "pending.json is written");
  g_lines.clear();
  Check(sentinel::ArmUpdateCrashMarker(root.c_str()), "the root arms the marker without a log");
  Check(g_lines.size() == 1, "no log: one logcat record at arm time");
  if (g_lines.size() == 1) {
    Check(g_lines[0].priority == ANDROID_LOG_WARN, "no log: the arm-time record is at Warning");
    Check(g_lines[0].text.find("[NEVR.HOST] crash_marker record unavailable reason=log_not_opened errno=" + std::to_string(ENOTDIR)) == 0,
          "no log: the record names the reason and the errno");
  }
  g_lines.clear();
  Check(sentinel::MarkUpdateCrashed() == sentinel::CrashMarkerOutcome::kWritten, "no log: the outcome is still written");
  Check(EntryExists(root + "/update/crashed"), "no log: update/crashed is still there");
  Check(g_lines.empty(), "no log: the crash path writes nothing to logcat");
  Check(ReadLog(root).files == 0, "no log: no log file was made");
  RemoveRoot(root);
}

// The record's time comes from integer arithmetic on the clock, not from gmtime: each instant reads as its UTC date.
void TestTimestampsAreFormattedByHand() {
  const std::string root = MakeRootWithLog();
  Check(!root.empty(), "a root can be made");
  if (root.empty()) return;
  Check(sentinel::ArmUpdateCrashMarker(root.c_str()), "the root arms the marker");
  for (const StampCase& stamp : kStampCases) {
    char line[kLineBytes];
    timespec now{};
    now.tv_sec = stamp.seconds;
    now.tv_nsec = stamp.nanos;
    const std::size_t size = sentinel::FormatMarkerRecord(line, sizeof(line), sentinel::CrashMarkerOutcome::kWritten, 0, kFakeThread, now);
    Check(size > 0, std::string("a record is formatted for ") + stamp.expected);
    if (size == 0) continue;
    Check(line[size - 1] == '\n' && std::strchr(line, '\n') == line + size - 1, std::string("one line ending in a newline for ") + stamp.expected);
    const nlohmann::json record = nlohmann::json::parse(std::string(line, size), nullptr, false);
    Check(!record.is_discarded() && IsString(record, "ts", stamp.expected), std::string("the record's ts reads ") + stamp.expected);
    Check(IsInteger(record, "thread", static_cast<long long>(kFakeThread)), "the thread is the number given");
  }
  char tiny[kTinyBytes];
  timespec now{};
  Check(sentinel::FormatMarkerRecord(tiny, sizeof(tiny), sentinel::CrashMarkerOutcome::kWritten, 0, kFakeThread, now) == 0,
        "a buffer too small for the record formats nothing");
  RemoveRoot(root);
}

}  // namespace

namespace sentinel {
// FinishCrashReport's report naming calls DumpDir; this test never reaches that call. Nothing here is a seam into the marker.
const char* DumpDir() { return ""; }
}  // namespace sentinel

extern "C" int __android_log_write(int priority, const char* /*tag*/, const char* text) {
  g_lines.push_back(Line{priority, text != nullptr ? text : ""});
  return 1;
}

extern "C" int __android_log_print(int, const char*, const char*, ...) { return 1; }

int main() {
  TestCrashWithPendingLeavesTheMarker();
  TestCrashWithoutPendingLeavesNothing();
  TestNotArmedWritesNothing();
  TestArmedWithoutPendingWritesNothing();
  TestWrittenMarkerAndItsRecord();
  TestRecordSharesTheLogFile();
  TestFailedMarkerNamesErrno();
  TestSymlinkMarkerIsRefused();
  TestRootTooLongDisarms();
  TestTrailingSeparatorRoot();
  TestPathWithSpecialCharactersIsEscaped();
  TestUnavailableLogLeavesTheMarkerWorking();
  TestTimestampsAreFormattedByHand();
  if (g_fail == 0) {
    std::printf("update_crash_marker_test: all checks pass\n");
    return 0;
  }
  std::printf("update_crash_marker_test: %d failure(s)\n", g_fail);
  return 1;
}

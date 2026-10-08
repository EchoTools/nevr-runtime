// Host test for the sentinel's configuration activation and logging (activation.cpp,
// sentinel_log.cpp) against a stand-in for liblog. HostCtor has constructor priority 102, which
// the loader runs before every default-priority static initializer, so it runs before
// activation.cpp's regardless of link order, the same relation as entry.cpp's constructor to the
// sentinel sources' initializers.
//
// Compile with -DNEVR_QUEST_FILES_DIR="<dir>" (a scratch directory this test creates and removes).
#include "activation.h"
#include "sentinel_log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NEVR_QUEST_FILES_DIR
#error "compile with -DNEVR_QUEST_FILES_DIR=\"<scratch dir>\""
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                \
    }                                                                              \
  } while (0)

struct Line {
  int prio;
  std::string text;
};

// Function-local so the constructor below may log before any namespace-scope object here exists.
std::vector<Line>& Captured() {
  static std::vector<Line> lines;
  return lines;
}

// A function, not a namespace-scope string: the constructor below runs before this files own
// dynamic initializers.
std::string Dir() { return NEVR_QUEST_FILES_DIR; }
const char* const kSecrets[] = {"FILE-SECRET-KEY-31337", "FILE-SERVER-SECRET-42042",
                                "STUB-EMBEDDED-API-SECRET-5521", "STUB-EMBEDDED-SERVER-SECRET-9973",
                                "file.example", "stub-emb.example",
                                // word- and hex-shaped markers: key names of this shape must not be echoed either
                                "5f4dcc3b5aa765d61d8327deb882cf99", "defaultkey", "s3cr3tlowercase"};

// Every line is checked the moment it is emitted, for the whole run, not at fixed points.
std::vector<std::string>& Leaks() {
  static std::vector<std::string> leaks;
  return leaks;
}

void Record(int prio, const std::string& text) {
  for (const char* secret : kSecrets) {
    if (text.find(secret) != std::string::npos) Leaks().push_back(text);
  }
  Captured().push_back({prio, text});
}

std::vector<std::string> ListDir() {
  std::vector<std::string> names;
  if (DIR* d = ::opendir(Dir().c_str())) {
    while (const dirent* e = ::readdir(d)) names.push_back(e->d_name);
    ::closedir(d);
  }
  return names;
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

int Count(const std::string& needle) {
  int n = 0;
  for (const Line& l : Captured()) {
    if (l.text.find(needle) != std::string::npos) ++n;
  }
  return n;
}

// A blocked constructor or test must fail the run, not hang it: armed before every other constructor.
void OnAlarm(int) {
  static const char msg[] = "sentinel_host_test: TIMEOUT, something blocked\n";
  const ssize_t ignored = ::write(2, msg, sizeof(msg) - 1);
  (void)ignored;
  ::_exit(3);
}

__attribute__((constructor(101))) void ArmAlarm() {
  std::signal(SIGALRM, OnAlarm);
  ::alarm(30);
}

// Mirrors entry.cpp: the ELF constructor resolves the configuration before main.
__attribute__((constructor(102))) void HostCtor() {
  ::mkdir(Dir().c_str(), 0755);
  // Stale leftovers from an interrupted earlier run.
  for (const char* stale : {"/nevr-sentinel.log", "/fifo-config", "/big.json"}) ::unlink((Dir() + stale).c_str());
  WriteFile(Dir() + "/nevr-quest.json",
            R"({"nevr_socket_uri":"wss://file.example/nevr","nevr_http_key":"FILE-SECRET-KEY-31337",)"
            R"("nevr_server_key":"FILE-SERVER-SECRET-42042",)"
            R"("features":{"redirect":true,"bridge":true,"login":true}})");
  sentinel::InitActivation();
}

ssize_t ThreeBytesAtATime(int, const void*, std::size_t n) { return static_cast<ssize_t>(n < 3 ? n : 3); }

std::string g_collected;
ssize_t CollectThree(int, const void* data, std::size_t n) {
  const std::size_t k = n < 3 ? n : 3;
  g_collected.append(static_cast<const char*>(data), k);
  return static_cast<ssize_t>(k);
}

int g_interruptCalls = 0;
ssize_t InterruptOnce(int, const void*, std::size_t n) {
  if (g_interruptCalls++ == 0) {
    errno = EINTR;
    return -1;
  }
  return static_cast<ssize_t>(n);
}

ssize_t ZeroBytes(int, const void*, std::size_t) { return 0; }

ssize_t NoSpace(int, const void*, std::size_t) {
  errno = ENOSPC;
  return -1;
}

void ConfigSurvivesStaticInitialisation() {
  // The constructor resolved the file; static initializers that ran after it must not have reset it.
  const nevr_quest::ResolvedConfig& c = sentinel::ActiveConfig();
  CHECK(c.socketUri.text == "wss://file.example/nevr");
  CHECK(c.socketUri.source == nevr_quest::Source::kFile);
  CHECK(c.effective.redirect && c.effective.bridge && c.effective.login);
  CHECK(sentinel::FeatureEnabled(nevr_quest::Feature::kRedirect));
  CHECK(sentinel::FeatureEnabled(nevr_quest::Feature::kLogin));
  CHECK(Count("feature=login requested=on effective=on") == 1);
}

void ScanDiskLogForValues(const char* when) {
  const std::string disk = ReadAll(Dir() + "/nevr-sentinel.log");
  for (const char* secret : kSecrets) {
    if (disk.find(secret) != std::string::npos) {
      std::fprintf(stderr, "disk log holds a configured value (%s) at: %s\n", secret, when);
      ++g_failures;
    }
  }
}

void NoLogLineCarriesAValue() {
  CHECK(!Captured().empty());
  ScanDiskLogForValues("after the constructor");
  CHECK(!ReadAll(Dir() + "/nevr-sentinel.log").empty());
}

void DiskLogIsOneJsonObjectPerLine() {
  const std::string disk = ReadAll(Dir() + "/nevr-sentinel.log");
  std::size_t start = 0;
  std::size_t lines = 0;
  while (start < disk.size()) {
    const std::size_t end = disk.find('\n', start);
    CHECK(end != std::string::npos);
    if (end == std::string::npos) break;
    const nlohmann::json j = nlohmann::json::parse(disk.substr(start, end - start), nullptr, false);
    CHECK(j.is_object());
    if (j.is_object()) {
      CHECK(j.value("tag", "") == "NEVR-Sentinel");
      CHECK(j["ts_unix_ms"].is_number_integer());
      CHECK(j.contains("level") && j.contains("msg"));
    }
    ++lines;
    start = end + 1;
  }
  CHECK(lines == Captured().size());
  CHECK(disk.find('\x1b') == std::string::npos);
}

void FormatEscapesHostileMessages() {
  const std::string msg = std::string("quote\" backslash\\ newline\n esc\x1b[31m tab\t ") + "bad\xff" + "utf8";
  const std::string line = nevr_quest::FormatDiskLogLine(nevr_quest::LogLevel::kWarn, 1234567890123LL, msg);
  CHECK(!line.empty() && line.back() == '\n');
  CHECK(line.find('\n') == line.size() - 1);
  CHECK(line.find('\x1b') == std::string::npos);
  const nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
  CHECK(j.is_object());
  if (!j.is_object()) return;
  CHECK(j.value("level", "") == "WARN" && j.value("ts_unix_ms", 0LL) == 1234567890123LL);
  const std::string back = j.value("msg", "");
  CHECK(back.find("quote\" backslash\\ newline\n esc\x1b[31m tab\t ") == 0);
}

void ReadConfigFileNeverBlocksOnNonRegularFiles() {
  std::string out;
  int err = 0;
  const std::string fifo = Dir() + "/fifo-config";
  CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
  CHECK(sentinel::ReadConfigFile(fifo, &out, &err) == sentinel::ReadStatus::kNotRegularFile);
  CHECK(sentinel::ReadConfigFile(Dir(), &out, &err) == sentinel::ReadStatus::kNotRegularFile);
  CHECK(sentinel::ReadConfigFile(Dir() + "/missing.json", &out, &err) == sentinel::ReadStatus::kAbsent);
  CHECK(err == ENOENT);

  Captured().clear();
  const nevr_quest::ResolvedConfig c = sentinel::ResolveFromDisk(fifo);
  CHECK(c.socketUri.source == nevr_quest::Source::kEmbedded);
  CHECK(!c.effective.redirect && !c.effective.bridge && !c.effective.login);
  CHECK(Count("is not a regular file") == 1);
  ::unlink(fifo.c_str());

  const std::string big = Dir() + "/big.json";
  WriteFile(big, std::string(nevr_quest::kMaxConfigBytes * 3, 'x'));
  CHECK(sentinel::ReadConfigFile(big, &out, &err) == sentinel::ReadStatus::kRead);
  CHECK(out.size() > nevr_quest::kMaxConfigBytes && out.size() <= nevr_quest::kMaxConfigBytes + 4096);
  const nevr_quest::ResolvedConfig rejected = sentinel::ResolveFromDisk(big);
  CHECK(rejected.socketUri.source == nevr_quest::Source::kEmbedded && !rejected.effective.redirect);
  ::unlink(big.c_str());

  // Rejected and invalid-value files that carry configured values: none may reach a log line.
  const std::string bad = Dir() + "/bad.json";
  WriteFile(bad, R"({"nevr_http_key":"FILE-SECRET-KEY-31337","nevr_server_key":"FILE-SERVER-SECRET-42042")");
  (void)sentinel::ResolveFromDisk(bad);
  WriteFile(bad, R"({"nevr_socket_uri":"http://file.example/wrong","nevr_http_key":"has space FILE-SECRET-KEY-31337"})");
  (void)sentinel::ResolveFromDisk(bad);
  WriteFile(bad, R"({"FILE-SECRET-KEY-31337":1,"features":{"login":"FILE-SERVER-SECRET-42042"}})");
  (void)sentinel::ResolveFromDisk(bad);
  WriteFile(bad, R"({"5f4dcc3b5aa765d61d8327deb882cf99":"x","defaultkey":1,"defaultkey":2,"features":{"s3cr3tlowercase":true}})");
  (void)sentinel::ResolveFromDisk(bad);
  ::unlink(bad.c_str());
  ScanDiskLogForValues("after rejected files");
}

void WriteAllRetriesAndReportsFailure() {
  int err = 0;
  const std::string payload = "0123456789abcdef";
  CHECK(sentinel::WriteAll(-1, payload.data(), payload.size(), &err, ThreeBytesAtATime));
  g_collected.clear();
  CHECK(sentinel::WriteAll(-1, payload.data(), payload.size(), &err, CollectThree));
  CHECK(g_collected == payload);
  g_interruptCalls = 0;
  CHECK(sentinel::WriteAll(-1, payload.data(), payload.size(), &err, InterruptOnce));
  CHECK(g_interruptCalls == 2);
  CHECK(!sentinel::WriteAll(-1, payload.data(), payload.size(), &err, ZeroBytes) && err == EIO);
  CHECK(!sentinel::WriteAll(-1, payload.data(), payload.size(), &err, NoSpace) && err == ENOSPC);
}

long long g_fakeNow = 0;
long long FakeNow() { return g_fakeNow; }

std::string LogPath() { return Dir() + "/nevr-sentinel.log"; }

std::vector<std::string> RotatedNames() {
  std::vector<std::string> names;
  for (const std::string& name : ListDir()) {
    if (name.rfind("nevr-sentinel.", 0) == 0 && name != "nevr-sentinel.log") names.push_back(name);
  }
  return names;
}

void ClearLogs() {
  sentinel::CloseDiskLog();
  ::unlink(LogPath().c_str());
  for (const std::string& name : RotatedNames()) ::unlink((Dir() + "/" + name).c_str());
}

std::size_t CountLines(const std::string& text) {
  std::size_t n = 0;
  for (const char c : text) n += c == '\n' ? 1 : 0;
  return n;
}

struct rlimit g_savedFsize {};

void RefuseAllWrites() {
  std::signal(SIGXFSZ, SIG_IGN);  // EFBIG instead of a signal
  CHECK(::getrlimit(RLIMIT_FSIZE, &g_savedFsize) == 0);
  struct rlimit zero = g_savedFsize;
  zero.rlim_cur = 0;
  CHECK(::setrlimit(RLIMIT_FSIZE, &zero) == 0);
}

void AllowWrites() { CHECK(::setrlimit(RLIMIT_FSIZE, &g_savedFsize) == 0); }

void TornRecordsStayOnTheirOwnLine() {
  static std::string out;
  out.clear();
  static int calls;
  calls = 0;
  const auto tearThenSucceed = [](int, const void* data, std::size_t n) -> ssize_t {
    if (calls++ == 0) {
      out.append(static_cast<const char*>(data), 5);
      return 5;
    }
    if (calls == 2) {
      errno = ENOSPC;
      return -1;
    }
    out.append(static_cast<const char*>(data), n);
    return static_cast<ssize_t>(n);
  };
  bool torn = false;
  int err = 0;
  const std::string first = nevr_quest::FormatDiskLogLine(nevr_quest::LogLevel::kInfo, 1, "first record");
  const std::string second = nevr_quest::FormatDiskLogLine(nevr_quest::LogLevel::kInfo, 2, "second record");
  CHECK(!sentinel::WriteRecord(-1, first, &torn, &err, tearThenSucceed) && torn && err == ENOSPC);
  CHECK(sentinel::WriteRecord(-1, second, &torn, &err, tearThenSucceed) && !torn);
  // The fragment is alone on its line and the next record parses.
  const std::size_t nl = out.find('\n');
  CHECK(nl == 5);
  const std::size_t end = out.find('\n', nl + 1);
  CHECK(end != std::string::npos);
  const nlohmann::json j = nlohmann::json::parse(out.substr(nl + 1, end - nl - 1), nullptr, false);
  CHECK(j.is_object() && j.value("msg", "") == "second record");

  // A write that put nothing in the file is not a torn record, and does not clear a real one.
  bool tornFlag = false;
  CHECK(!sentinel::WriteRecord(-1, first, &tornFlag, &err, [](int, const void*, std::size_t) -> ssize_t {
    errno = EFBIG;
    return -1;
  }));
  CHECK(!tornFlag && err == EFBIG);
  tornFlag = true;
  CHECK(!sentinel::WriteRecord(-1, first, &tornFlag, &err, [](int, const void*, std::size_t) -> ssize_t {
    errno = EFBIG;
    return -1;
  }));
  CHECK(tornFlag);
}

void NothingWrittenLeavesNoBlankLine() {
  ClearLogs();
  g_fakeNow = 1000;
  Captured().clear();
  RefuseAllWrites();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "refused");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "refused again");
  AllowWrites();
  CHECK(Count("on-disk log write failed") == 1);  // reported once for two failed writes
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "accepted");
  const std::string disk = ReadAll(LogPath());
  CHECK(!disk.empty() && disk[0] == '{');
  CHECK(CountLines(disk) == 1);
  const nlohmann::json j = nlohmann::json::parse(disk, nullptr, false);
  CHECK(j.is_object() && j.value("msg", "") == "accepted");

  // A torn record left by an earlier process: the next record starts a new line.
  ClearLogs();
  WriteFile(LogPath(), R"({"ts_unix_ms":1,"level":"INFO","msg":"torn)");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "after the fragment");
  const std::string after = ReadAll(LogPath());
  CHECK(CountLines(after) == 2);
  const std::size_t nl = after.find('\n');
  CHECK(nl != std::string::npos);
  const nlohmann::json k = nlohmann::json::parse(after.substr(nl + 1), nullptr, false);
  CHECK(k.is_object() && k.value("msg", "") == "after the fragment");
  ClearLogs();
}

void LargeLogIsRotatedNotDeleted() {
  ClearLogs();
  g_fakeNow = 5000;
  const std::size_t big = static_cast<std::size_t>(sentinel::kMaxDiskLogBytes) + 1;
  WriteFile(LogPath(), std::string(big, 'x'));
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "after rotation");
  CHECK(ReadAll(LogPath()).size() < 1024);
  CHECK(ReadAll(LogPath()).find("after rotation") != std::string::npos);
  CHECK(RotatedNames().size() == 1);
  CHECK(ReadAll(Dir() + "/nevr-sentinel.5000.log").size() == big);

  // A second rotation in the same millisecond must not replace the first rotated file.
  sentinel::CloseDiskLog();
  WriteFile(LogPath(), std::string(big + 7, 'y'));
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "after second rotation");
  CHECK(RotatedNames().size() == 2);
  CHECK(ReadAll(Dir() + "/nevr-sentinel.5000.log").size() == big);
  CHECK(ReadAll(Dir() + "/nevr-sentinel.5000.1.log").size() == big + 7);
  ClearLogs();
}

void LogIsRotatedInProcessAtTheBound() {
  ClearLogs();
  g_fakeNow = 7000;
  const std::string message(1000, 'm');
  const int total = 1100;  // about 1.1 MiB
  for (int i = 0; i < total; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, message);
  const std::vector<std::string> rotated = RotatedNames();
  CHECK(rotated.size() == 1);
  std::size_t lines = CountLines(ReadAll(LogPath()));
  for (const std::string& name : rotated) lines += CountLines(ReadAll(Dir() + "/" + name));
  CHECK(lines == static_cast<std::size_t>(total));
  CHECK(ReadAll(LogPath()).size() < static_cast<std::size_t>(sentinel::kMaxDiskLogBytes));
  ClearLogs();
}

void EachDiskFailureClassIsReportedOnce() {
  ClearLogs();
  ::unlink((Dir() + "/nevr-quest.json").c_str());
  CHECK(::rmdir(Dir().c_str()) == 0);
  g_fakeNow = 100000;

  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "first line");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "second line");
  CHECK(Count("on-disk log open failed") == 1);
  CHECK(Count("first line") == 1 && Count("second line") == 1);

  CHECK(::mkdir(Dir().c_str(), 0755) == 0);
  g_fakeNow += sentinel::kOpenRetryMs + 1000;  // past the retry throttle
  RefuseAllWrites();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "third line");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "fourth line");
  AllowWrites();
  CHECK(Count("on-disk log write failed") == 0);  // the write class was already reported once this run
  CHECK(Count("on-disk log open failed") == 1);
  CHECK(Count("third line") == 1 && Count("fourth line") == 1);
  ClearLogs();
}

void RegularLogFileIsBlockingAfterOpen() {
  ClearLogs();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "open the log");
  bool checked = false;
  for (int fd = 3; fd < 256; ++fd) {
    char link[64];
    char target[512];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = ::readlink(link, target, sizeof(target) - 1);
    if (n <= 0) continue;
    target[n] = '\0';
    if (LogPath() != target) continue;
    checked = true;
    const int flags = ::fcntl(fd, F_GETFL);
    CHECK(flags >= 0 && (flags & O_NONBLOCK) == 0);
  }
  CHECK(checked);
  ClearLogs();
}

void HostileLogPathsAreRetriedRarelyOrNever() {
  ClearLogs();
  g_fakeNow = 200000;

  // A missing directory: one attempt per throttle window, and logging recovers once it exists.
  CHECK(::rmdir(Dir().c_str()) == 0);
  unsigned before = sentinel::OpenAttemptsForTest();
  for (int i = 0; i < 5; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, "missing dir");
  CHECK(sentinel::OpenAttemptsForTest() == before + 1);
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "missing dir later");
  CHECK(sentinel::OpenAttemptsForTest() == before + 2);
  CHECK(::mkdir(Dir().c_str(), 0755) == 0);
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "recovered");
  CHECK(ReadAll(LogPath()).find("recovered") != std::string::npos);
  ClearLogs();

  // A directory at the log path.
  CHECK(::mkdir(LogPath().c_str(), 0755) == 0);
  before = sentinel::OpenAttemptsForTest();
  for (int i = 0; i < 5; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, "directory");
  CHECK(sentinel::OpenAttemptsForTest() == before + 1);
  ::rmdir(LogPath().c_str());
  sentinel::CloseDiskLog();

  // A dangling symlink is not followed and its target is not created.
  const std::string target = Dir() + "/symlink-target-never-created";
  CHECK(::symlink(target.c_str(), LogPath().c_str()) == 0);
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "dangling symlink");
  CHECK(::access(target.c_str(), F_OK) != 0);
  ::unlink(LogPath().c_str());
  sentinel::CloseDiskLog();

  // A FIFO with no reader, then with a reader: never blocks, disabled after the first look, nothing
  // is written into it.
  CHECK(::mkfifo(LogPath().c_str(), 0600) == 0);
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  Captured().clear();
  before = sentinel::OpenAttemptsForTest();
  for (int i = 0; i < 5; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, "fifo no reader");
  CHECK(sentinel::OpenAttemptsForTest() == before + 1);
  CHECK(Count("fifo no reader") == 5);
  CHECK(Count("on-disk log open failed: path is not a regular file") == 1);
  sentinel::CloseDiskLog();
  const int reader = ::open(LogPath().c_str(), O_RDONLY | O_NONBLOCK);
  CHECK(reader >= 0);
  for (int i = 0; i < 5; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, "fifo with reader");
  char buf[64];
  CHECK(::read(reader, buf, sizeof(buf)) <= 0);
  ::close(reader);
  ::unlink(LogPath().c_str());
  ClearLogs();
}

}  // namespace

extern "C" {

int __android_log_write(int prio, const char*, const char* text) {
  Record(prio, text);
  return 1;
}

int __android_log_print(int prio, const char*, const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Record(prio, buf);
  return 1;
}

}  // extern "C"

int main() {
  ConfigSurvivesStaticInitialisation();
  NoLogLineCarriesAValue();
  DiskLogIsOneJsonObjectPerLine();
  FormatEscapesHostileMessages();
  ReadConfigFileNeverBlocksOnNonRegularFiles();
  WriteAllRetriesAndReportsFailure();
  sentinel::SetClockForTest(FakeNow);
  TornRecordsStayOnTheirOwnLine();
  NothingWrittenLeavesNoBlankLine();
  LargeLogIsRotatedNotDeleted();
  LogIsRotatedInProcessAtTheBound();
  EachDiskFailureClassIsReportedOnce();
  RegularLogFileIsBlockingAfterOpen();
  HostileLogPathsAreRetriedRarelyOrNever();
  ::rmdir(Dir().c_str());
  CHECK(Leaks().empty());
  for (const std::string& leak : Leaks()) std::fprintf(stderr, "a log line carried a configured value: %s\n", leak.c_str());
  if (g_failures != 0) {
    std::fprintf(stderr, "sentinel_host_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("sentinel_host_test: all checks pass\n");
  return 0;
}

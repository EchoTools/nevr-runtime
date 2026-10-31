// Host test for the sentinel's configuration activation and logging (activation.cpp,
// sentinel_log.cpp) against a stand-in for liblog. HostCtor has constructor priority 102, which
// the loader runs before every default-priority static initializer, so it runs before
// activation.cpp's regardless of link order, the same relation as entry.cpp's constructor to the
// sentinel sources' initializers.
//
// Compile with -DNEVR_QUEST_FILES_DIR="<dir>" (a scratch directory this test creates and removes).
#include "activation.h"
#include "sentinel_log.h"
#include "quest/sentinel/crash_dir.h"
#include "quest/sentinel/data_root.h"

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
#include <climits>
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
  for (const char* stale : {"/nevr-sentinel.log", "/fifo-config", "/big.yaml", "/bad.yaml", "/locked.yaml", "/absent-dir/config.yaml"}) {
    ::unlink((Dir() + stale).c_str());
  }
  WriteFile(Dir() + "/config.yaml",
            "services:\n  socket_uri: \"wss://file.example/nevr\"\n"
            "auth:\n  http_key: \"FILE-SECRET-KEY-31337\"\n  server_key: \"FILE-SERVER-SECRET-42042\"\n");
  // A file left over from the JSON configuration: it is not read, whatever it says, and is reported once.
  WriteFile(Dir() + "/nevr-quest.json", R"({"nevr_socket_uri":"wss://legacy.example/nevr","features":{"social":false}})");
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
  CHECK(sentinel::FeatureEnabled(nevr_quest::Feature::kSocial));
  CHECK(Count("feature=social requested=on effective=on") == 1);
  CHECK(Count("feature=login requested=on effective=on") == 1);
  // The sentinel log names the file it read.
  CHECK(Count("config file=" + Dir() + "/config.yaml loaded") == 1);
  CHECK(Count("absent, using embedded defaults") == 0);
}

void ALeftoverJsonConfigIsReportedAndNeverRead() {
  // Warn priority (Android log priority 5), once, naming both files; its content (social:false, another socket
  // URI) changed nothing.
  int warns = 0;
  for (const Line& l : Captured()) {
    if (l.text.find("nevr-quest.json present and ignored; settings come from config.yaml") != std::string::npos) {
      ++warns;
      CHECK(l.prio == 5);
    }
  }
  CHECK(warns == 1);
  CHECK(sentinel::ActiveConfig().socketUri.text == "wss://file.example/nevr");
  CHECK(sentinel::ActiveConfig().effective.social);
  CHECK(Count("legacy.example") == 0);
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
  const std::string line = sentinel::FormatDiskLogLine(nevr_quest::LogLevel::kWarn, 1234567890123LL, msg);
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

void StructuredDiskLogIsFlat() {
  const std::string line = sentinel::FormatDiskLogLine(
      nevr_quest::LogLevel::kInfo, 1234567890123LL,
      R"({"ts_ms":1234567890000,"level":"info","event":"obb_skip_install","result":"installed"})");
  const nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
  CHECK(j.is_object());
  if (!j.is_object()) return;
  CHECK(j.value("event", "") == "obb_skip_install");
  CHECK(j.value("result", "") == "installed");
  CHECK(j.value("level", "") == "info");
  CHECK(j.value("tag", "") == "NEVR-Sentinel");
  CHECK(j.value("ts_unix_ms", 0LL) == 1234567890123LL);
  CHECK(!j.contains("ts_ms") && !j.contains("msg"));
}

void ReadConfigFileNeverBlocksOnNonRegularFiles() {
  std::string out;
  int err = 0;
  const std::string fifo = Dir() + "/fifo-config";
  CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
  CHECK(sentinel::ReadConfigFile(fifo, &out, &err) == sentinel::ReadStatus::kNotRegularFile);
  CHECK(sentinel::ReadConfigFile(Dir(), &out, &err) == sentinel::ReadStatus::kNotRegularFile);
  CHECK(sentinel::ReadConfigFile(Dir() + "/missing.yaml", &out, &err) == sentinel::ReadStatus::kAbsent);
  CHECK(err == ENOENT);

  Captured().clear();
  const nevr_quest::ResolvedConfig c = sentinel::ResolveFromDisk(fifo);
  CHECK(c.socketUri.source == nevr_quest::Source::kEmbedded);
  CHECK(c.effective.redirect && c.effective.bridge && c.effective.login);  // the embedded defaults carry them
  CHECK(Count("config file=" + fifo + " is not a regular file, not read, using embedded defaults") == 1);
  ::unlink(fifo.c_str());

  // A path with no file: one Info line naming it, the embedded defaults, no Error.
  Captured().clear();
  const std::string none = Dir() + "/absent-dir/config.yaml";
  const nevr_quest::ResolvedConfig absent = sentinel::ResolveFromDisk(none);
  CHECK(absent.socketUri.source == nevr_quest::Source::kEmbedded && absent.effective.login);
  CHECK(Count("config file=" + none + " absent, using embedded defaults") == 1);
  CHECK(Count("nevr-quest.json present") == 0);

  // A file that exists but cannot be read: reported with its errno, the embedded defaults used, nothing of it read.
  if (::geteuid() != 0) {  // root reads a mode-0000 file
    const std::string locked = Dir() + "/locked.yaml";
    WriteFile(locked, "social:\n  facade: false\n");
    CHECK(::chmod(locked.c_str(), 0000) == 0);
    Captured().clear();
    const nevr_quest::ResolvedConfig unreadable = sentinel::ResolveFromDisk(locked);
    CHECK(unreadable.effective.social);  // the facade: false inside was not read
    CHECK(Count("config file=" + locked + " unreadable errno=13 (Permission denied), using embedded defaults") == 1);
    ::chmod(locked.c_str(), 0600);
    ::unlink(locked.c_str());
  }

  const std::string big = Dir() + "/big.yaml";
  WriteFile(big, std::string(nevr_quest::kMaxConfigBytes * 3, 'x'));
  CHECK(sentinel::ReadConfigFile(big, &out, &err) == sentinel::ReadStatus::kRead);
  CHECK(out.size() > nevr_quest::kMaxConfigBytes && out.size() <= nevr_quest::kMaxConfigBytes + 4096);
  Captured().clear();
  const nevr_quest::ResolvedConfig rejected = sentinel::ResolveFromDisk(big);
  CHECK(rejected.socketUri.source == nevr_quest::Source::kEmbedded && rejected.effective.redirect);
  CHECK(Count("config file=" + big + " rejected reason=") == 1);
  ::unlink(big.c_str());

  // Rejected and invalid-value files that carry configured values: none may reach a log line.
  const std::string bad = Dir() + "/bad.yaml";
  WriteFile(bad, "auth:\n  http_key: \"FILE-SECRET-KEY-31337\"\n  server_key: \"FILE-SERVER-SECRET-42042\"\n  x: [\n");
  Captured().clear();
  static_cast<void>(sentinel::ResolveFromDisk(bad));
  CHECK(Count("config file=" + bad + " rejected reason=yaml_syntax (embedded defaults stay)") == 1);
  WriteFile(bad, "services:\n  socket_uri: \"http://file.example/wrong\"\nauth:\n  http_key: \"has space FILE-SECRET-KEY-31337\"\n");
  static_cast<void>(sentinel::ResolveFromDisk(bad));
  WriteFile(bad, "FILE-SECRET-KEY-31337: 1\nauth:\n  server_key: FILE-SERVER-SECRET-42042\n");
  static_cast<void>(sentinel::ResolveFromDisk(bad));
  WriteFile(bad, "5f4dcc3b5aa765d61d8327deb882cf99: x\ndefaultkey: 1\nsocial:\n  s3cr3tlowercase: true\n");
  static_cast<void>(sentinel::ResolveFromDisk(bad));
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
  const std::string first = sentinel::FormatDiskLogLine(nevr_quest::LogLevel::kInfo, 1, "first record");
  const std::string second = sentinel::FormatDiskLogLine(nevr_quest::LogLevel::kInfo, 2, "second record");
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
  ::unlink((Dir() + "/config.yaml").c_str());
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
  char resolved[PATH_MAX];  // /proc/self/fd links are absolute; the scratch directory may be relative
  CHECK(::realpath(LogPath().c_str(), resolved) != nullptr);
  bool checked = false;
  for (int fd = 3; fd < 256; ++fd) {
    char link[64];
    char target[512];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = ::readlink(link, target, sizeof(target) - 1);
    if (n <= 0) continue;
    target[n] = '\0';
    if (std::string(resolved) != target) continue;
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

// Rotation that cannot rename (#238): the failure is logged once, not once per record, the rename is
// not attempted again inside the retry window, and the log is not reopened for every record.
int g_renameCalls = 0;
int FailingRename(const char*, const char*) {
  ++g_renameCalls;
  errno = EACCES;
  return -1;
}

void RotationFailureIsThrottled() {
  ClearLogs();
  g_fakeNow = 400000;
  g_renameCalls = 0;
  sentinel::SetRenameForTest(&FailingRename);
  const std::size_t big = static_cast<std::size_t>(sentinel::kMaxDiskLogBytes) + 1;
  WriteFile(LogPath(), std::string(big, 'x'));
  Captured().clear();
  const unsigned before = sentinel::OpenAttemptsForTest();
  for (int i = 0; i < 50; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, "rotation blocked");
  CHECK(Count("on-disk log rotation failed") == 1);
  CHECK(g_renameCalls == 1);
  CHECK(sentinel::OpenAttemptsForTest() == before + 1);
  CHECK(ReadAll(LogPath()).size() > big);  // the log kept growing and nothing was lost
  CHECK(RotatedNames().empty());

  // Past the window one more attempt is made, and it is not reported a second time.
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  for (int i = 0; i < 5; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, "rotation blocked later");
  CHECK(g_renameCalls == 2);
  CHECK(Count("on-disk log rotation failed") == 1);

  // The rename works again: the log rotates at the next attempt, and a later failure is reported again.
  sentinel::SetRenameForTest(nullptr);
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  for (int i = 0; i < 5; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, "rotation recovered");
  CHECK(RotatedNames().size() == 1);
  CHECK(ReadAll(LogPath()).size() < 1024);
  sentinel::SetRenameForTest(nullptr);
  ClearLogs();
}

// The retry interval runs on the monotonic clock (#238): stepping the wall clock backwards does not
// reopen the throttle window. The wall clock only stamps records and names rotated files.
long long g_fakeWall = 0;
long long FakeWall() { return g_fakeWall; }

void RetryIntervalIgnoresTheWallClock() {
  ClearLogs();
  g_fakeNow = 500000;
  g_fakeWall = 900000;
  sentinel::SetWallClockForTest(FakeWall);
  CHECK(::rmdir(Dir().c_str()) == 0);
  const unsigned before = sentinel::OpenAttemptsForTest();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "wall clock first");
  CHECK(sentinel::OpenAttemptsForTest() == before + 1);
  g_fakeWall = 100000;  // stepped back: a throttle that read it would retry now
  for (int i = 0; i < 3; ++i) sentinel::Emit(nevr_quest::LogLevel::kInfo, "wall clock stepped back");
  CHECK(sentinel::OpenAttemptsForTest() == before + 1);
  g_fakeNow += sentinel::kOpenRetryMs + 1;  // the interval clock moves on: one retry
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "wall clock window passed");
  CHECK(sentinel::OpenAttemptsForTest() == before + 2);
  sentinel::SetWallClockForTest(nullptr);
  CHECK(::mkdir(Dir().c_str(), 0755) == 0);
  ClearLogs();
}

// What the header documents (#238): a symlink or a directory at the log path fails the open and is
// retried after the window; only a path that opens as a non-regular file is never retried.
void UnopenablePathsAreRetriedAfterTheWindow() {
  ClearLogs();
  g_fakeNow = 600000;
  const std::string target = Dir() + "/symlink-target-never-created";
  CHECK(::symlink(target.c_str(), LogPath().c_str()) == 0);
  unsigned before = sentinel::OpenAttemptsForTest();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "symlink first");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "symlink inside window");
  CHECK(sentinel::OpenAttemptsForTest() == before + 1);
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "symlink after window");
  CHECK(sentinel::OpenAttemptsForTest() == before + 2);
  CHECK(::access(target.c_str(), F_OK) != 0);
  ::unlink(LogPath().c_str());
  sentinel::CloseDiskLog();

  CHECK(::mkdir(LogPath().c_str(), 0755) == 0);
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  before = sentinel::OpenAttemptsForTest();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "directory first");
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "directory after window");
  CHECK(sentinel::OpenAttemptsForTest() == before + 2);
  ::rmdir(LogPath().c_str());
  ClearLogs();
}

// ---- the log folder (#1797): created below a trusted part, never through a link, chosen once ----

// A folder tree of the test, removed bottom-up. Names are relative to Dir().
void RemoveTree(const std::string& path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    ::chmod(path.c_str(), 0700);
    if (DIR* d = ::opendir(path.c_str())) {
      std::vector<std::string> names;
      while (const dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
      }
      ::closedir(d);
      for (const std::string& n : names) RemoveTree(path + "/" + n);
    }
    ::rmdir(path.c_str());
  } else {
    ::unlink(path.c_str());
  }
}

// The candidate strings live here for the whole test: the seam keeps pointers.
struct Cands {
  std::vector<std::string> names, dirs, trusted;
  std::vector<sentinel::LogDirCandidate> list;
  void Add(const std::string& name, const std::string& dir, const std::string& trust) {
    names.push_back(name);
    dirs.push_back(dir);
    trusted.push_back(trust);
  }
  void Install() {
    list.clear();
    for (std::size_t i = 0; i < names.size(); ++i) list.push_back({names[i].c_str(), dirs[i].c_str(), trusted[i].c_str()});
    sentinel::SetLogDirCandidatesForTest(list.data(), list.size());
  }
};

std::string Slurp(const std::string& path) { return ReadAll(path); }

// The captured lines whose JSON `event` is exactly `event`.
std::vector<nlohmann::json> Events(const std::string& event) {
  std::vector<nlohmann::json> found;
  for (const Line& l : Captured()) {
    const nlohmann::json j = nlohmann::json::parse(l.text, nullptr, false);
    if (j.is_object() && j.value("event", "") == event) {
      nlohmann::json one = j;
      one["_prio"] = l.prio;
      found.push_back(one);
    }
  }
  return found;
}

// A link target must not depend on where the link sits: the recipe's scratch folder is a relative path.
std::string Absolute(const std::string& path) {
  char resolved[PATH_MAX];
  return ::realpath(path.c_str(), resolved) != nullptr ? std::string(resolved) : path;
}

bool IsDir(const std::string& path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void TheLogFolderChainIsCreatedAndReportedOnce() {
  RemoveTree(Dir() + "/lf");
  g_fakeNow = 800000;
  Cands c;
  c.Add("first", Dir() + "/lf/a/b/files", Dir());  // lf, a and b do not exist
  c.Install();
  CHECK(sentinel::LogDir() == Dir() + "/lf/a/b/files");  // before any open: the first candidate
  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "into a made folder");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "second record");
  const std::string log = Dir() + "/lf/a/b/files/nevr-sentinel.log";
  CHECK(IsDir(Dir() + "/lf/a/b/files"));
  const std::string disk = Slurp(log);
  CHECK(disk.find("into a made folder") != std::string::npos);
  CHECK(sentinel::LogDir() == Dir() + "/lf/a/b/files");
  // One Info line, once, naming the folder and the winner; it is the first record on disk.
  const std::vector<nlohmann::json> chosen = Events("sentinel_log_dir");
  CHECK(chosen.size() == 1);
  for (const nlohmann::json& j : chosen) {
    CHECK(j.value("_prio", 0) == 4);  // ANDROID_LOG_INFO
    CHECK(j.value("candidate", "") == "first" && j.value("dir", "") == Dir() + "/lf/a/b/files");
    CHECK(!j.contains("errno"));
  }
  CHECK(Events("sentinel_log_dir_failed").empty());  // nothing failed, so nothing is reported failed
  const nlohmann::json firstLine = nlohmann::json::parse(disk.substr(0, disk.find('\n')), nullptr, false);
  CHECK(firstLine.is_object() && firstLine.value("event", "") == "sentinel_log_dir" && firstLine.value("level", "") == "INFO");
  // The config folder is not the log folder.
  CHECK(std::string(sentinel::FilesDir()) == Dir());

  // The rotated file lands in the log folder, not beside the config.
  const std::size_t big = static_cast<std::size_t>(sentinel::kMaxDiskLogBytes) + 1;
  sentinel::CloseDiskLog();
  WriteFile(log, std::string(big, 'z'));
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "after rotation in the made folder");
  CHECK(::access((Dir() + "/lf/a/b/files/nevr-sentinel.800000.log").c_str(), F_OK) == 0);
  CHECK(RotatedNames().empty());
  CHECK(Events("sentinel_log_dir").size() == 1);  // opening again after a rotation does not choose or name the folder again

  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/lf");
}

void ALinkBelowTheTrustedPartIsNotFollowed() {
  RemoveTree(Dir() + "/lk");
  RemoveTree(Dir() + "/elsewhere");
  g_fakeNow = 810000;
  CHECK(::mkdir((Dir() + "/lk").c_str(), 0755) == 0);
  CHECK(::mkdir((Dir() + "/elsewhere").c_str(), 0755) == 0);
  CHECK(::symlink(Absolute(Dir() + "/elsewhere").c_str(), (Dir() + "/lk/link").c_str()) == 0);
  Cands c;
  c.Add("through_link", Dir() + "/lk/link/files", Dir() + "/lk");
  c.Add("clean", Dir() + "/lk/real/files", Dir() + "/lk");
  c.Install();
  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "link refused");
  CHECK(::access((Dir() + "/elsewhere/files").c_str(), F_OK) != 0);  // nothing was made through the link
  CHECK(IsDir(Dir() + "/lk/real/files"));
  CHECK(Slurp(Dir() + "/lk/real/files/nevr-sentinel.log").find("link refused") != std::string::npos);
  // The skipped candidate is reported with its folder and errno (ENOTDIR: lstat says the link is not a directory),
  // at Warning, and the winner once at Info.
  const std::vector<nlohmann::json> failed = Events("sentinel_log_dir_failed");
  CHECK(failed.size() == 1);
  for (const nlohmann::json& j : failed) {
    CHECK(j.value("_prio", 0) == 5);  // ANDROID_LOG_WARN
    CHECK(j.value("candidate", "") == "through_link" && j.value("dir", "") == Dir() + "/lk/link/files");
    CHECK(j.value("errno", 0) == ENOTDIR);
    CHECK(j.value("error", "") == std::strerror(ENOTDIR));
  }
  const std::vector<nlohmann::json> won = Events("sentinel_log_dir");
  CHECK(won.size() == 1 && won[0].value("candidate", "") == "clean");

  // A trusted part may itself be a link (/sdcard is one on a headset): it is used, not refused.
  CHECK(::symlink(Absolute(Dir() + "/elsewhere").c_str(), (Dir() + "/lk/trusted-link").c_str()) == 0);
  Cands t;
  t.Add("under_trusted_link", Dir() + "/lk/trusted-link/made/files", Dir() + "/lk/trusted-link");
  t.Install();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "trusted link followed");
  CHECK(IsDir(Dir() + "/elsewhere/made/files"));
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/lk");
  RemoveTree(Dir() + "/elsewhere");
}

void AFirstCandidateThatCannotBeUsedFallsThrough() {
  RemoveTree(Dir() + "/fb");
  g_fakeNow = 820000;
  CHECK(::mkdir((Dir() + "/fb").c_str(), 0755) == 0);
  WriteFile(Dir() + "/fb/blocker", "a file where the folder should be");
  Cands c;
  c.Add("media", Dir() + "/fb/blocker/files", Dir() + "/fb");  // a file in the way: ENOTDIR
  c.Add("data", Dir() + "/fb/data/x/files", Dir() + "/fb");
  c.Install();
  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "falls through");
  CHECK(sentinel::LogDir() == Dir() + "/fb/data/x/files");
  CHECK(Slurp(Dir() + "/fb/data/x/files/nevr-sentinel.log").find("falls through") != std::string::npos);
  CHECK(Count("on-disk log open failed") == 0);  // the next candidate worked: nothing to report
  const std::vector<nlohmann::json> won = Events("sentinel_log_dir");
  const std::vector<nlohmann::json> failed = Events("sentinel_log_dir_failed");
  CHECK(won.size() == 1 && won[0].value("candidate", "") == "data" && won[0].value("dir", "") == Dir() + "/fb/data/x/files");
  CHECK(failed.size() == 1 && failed[0].value("candidate", "") == "media" &&
        failed[0].value("dir", "") == Dir() + "/fb/blocker/files" && failed[0].value("errno", 0) == ENOTDIR);
  // On disk too, in order: the failure, then the winner, then the record that opened the log.
  const std::string disk = Slurp(Dir() + "/fb/data/x/files/nevr-sentinel.log");
  const std::size_t atFailed = disk.find("\"event\":\"sentinel_log_dir_failed\"");
  const std::size_t atWon = disk.find("\"event\":\"sentinel_log_dir\"");
  const std::size_t atRecord = disk.find("falls through");
  CHECK(atFailed != std::string::npos && atWon != std::string::npos && atRecord != std::string::npos);
  CHECK(atFailed < atWon && atWon < atRecord);

  // A folder that exists but is not writable (root writes anywhere, so this part needs another user).
  if (::geteuid() != 0) {
    CHECK(::mkdir((Dir() + "/fb/ro").c_str(), 0500) == 0);
    Cands r;
    r.Add("media", Dir() + "/fb/ro/files", Dir() + "/fb");
    r.Add("data", Dir() + "/fb/rw/files", Dir() + "/fb");
    r.Install();
    sentinel::Emit(nevr_quest::LogLevel::kInfo, "read-only first candidate");
    CHECK(sentinel::LogDir() == Dir() + "/fb/rw/files");
    CHECK(!IsDir(Dir() + "/fb/ro/files"));
  }

  // Nothing usable: one open failure is reported and logcat keeps every line.
  Cands none;
  none.Add("media", Dir() + "/fb/blocker/files", Dir() + "/fb");
  none.Add("data", Dir() + "/fb/blocker/other", Dir() + "/fb");
  none.Install();
  Captured().clear();
  const unsigned attempts = sentinel::OpenAttemptsForTest();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "nowhere");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "nowhere again");
  CHECK(Count("nowhere") == 2);                                   // logcat keeps every line
  CHECK(sentinel::OpenAttemptsForTest() == attempts + 1);          // one try, then the retry throttle
  CHECK(Events("sentinel_log_dir").empty());                       // nothing was chosen, so nothing is named
  // Every candidate that was tried is reported with its folder and errno, once.
  std::vector<nlohmann::json> each = Events("sentinel_log_dir_failed");
  CHECK(each.size() == 2);
  if (each.size() == 2) {
    CHECK(each[0].value("candidate", "") == "media" && each[0].value("dir", "") == Dir() + "/fb/blocker/files" &&
          each[0].value("errno", 0) == ENOTDIR);
    CHECK(each[1].value("candidate", "") == "data" && each[1].value("dir", "") == Dir() + "/fb/blocker/other" &&
          each[1].value("errno", 0) == ENOTDIR);
  }
  g_fakeNow += sentinel::kOpenRetryMs + 1;  // the retry fails the same way: not reported again
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "nowhere later");
  CHECK(Events("sentinel_log_dir_failed").size() == 2);
  // The same candidate failing a different way is news again (root ignores the mode that makes it fail).
  if (::geteuid() != 0) {
    ::unlink((Dir() + "/fb/blocker").c_str());
    CHECK(::mkdir((Dir() + "/fb/blocker").c_str(), 0500) == 0);  // now a folder nobody may add to: EACCES
    g_fakeNow += sentinel::kOpenRetryMs + 1;
    sentinel::Emit(nevr_quest::LogLevel::kInfo, "nowhere differently");
    each = Events("sentinel_log_dir_failed");
    CHECK(each.size() == 4);
    if (each.size() == 4) CHECK(each[2].value("errno", 0) == EACCES && each[3].value("errno", 0) == EACCES);
  }

  // A candidate whose path is not under its trusted part is refused, never made.
  CHECK(::mkdir((Dir() + "/fb/abc").c_str(), 0755) == 0);  // so a wrong accept would make files below it
  Cands bad;
  bad.Add("bad", Dir() + "/fb/abc/files", Dir() + "/fb/xyz");  // same length up to the '/', different folder
  bad.Install();
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "bad candidate");
  CHECK(!IsDir(Dir() + "/fb/abc/files"));
  // The trusted part must end at a '/' of the path: "/fb/else" is not a parent of "/fb/elsewhere/files".
  Cands edge;
  edge.Add("edge", Dir() + "/fb/elsewhere/files", Dir() + "/fb/else");
  edge.Install();
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "edge candidate");
  CHECK(!IsDir(Dir() + "/fb/elsewhere"));
  // A path longer than the buffer is refused, not truncated into another folder.
  std::string deep = Dir() + "/fb";
  for (int i = 0; i < 250; ++i) deep += "/d";
  Cands longer;
  longer.Add("long", deep, Dir() + "/fb");
  longer.Install();
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "long candidate");
  CHECK(!IsDir(Dir() + "/fb/d"));
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/fb");
}

void TheFolderIsChosenAgainWhenItVanishes() {
  RemoveTree(Dir() + "/vn");
  g_fakeNow = 830000;
  Cands c;
  c.Add("only", Dir() + "/vn/a/files", Dir());
  c.Install();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "before");
  CHECK(IsDir(Dir() + "/vn/a/files"));
  RemoveTree(Dir() + "/vn");  // the user cleared the app's files
  sentinel::CloseDiskLog();
  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "gone");  // ENOENT: reported, throttled
  CHECK(Count("on-disk log open failed") == 0);        // the failure class was already reported once this run
  g_fakeNow += sentinel::kOpenRetryMs + 1;
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "back");
  CHECK(IsDir(Dir() + "/vn/a/files"));
  CHECK(Slurp(Dir() + "/vn/a/files/nevr-sentinel.log").find("\"back\"") != std::string::npos ||
        Slurp(Dir() + "/vn/a/files/nevr-sentinel.log").find("back") != std::string::npos);
  CHECK(Events("sentinel_log_dir").size() == 1);  // chosen again: named again, once
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/vn");
}


void TheShippedCandidatesAreMediaThenData() {
  std::size_t n = 0;
  const sentinel::LogDirCandidate* c = sentinel::ShippedLogDirCandidatesForTest(&n);
  CHECK(n == 2);
  if (n != 2) return;
  CHECK(std::string(c[0].name) == "android_media" && std::string(c[0].dir) == nevr_quest::data_root::kMediaRoot);
  CHECK(std::string(c[0].trusted) == "/sdcard/Android/media/com.readyatdawn.r15");
  CHECK(std::string(c[1].name) == "android_data" &&
        std::string(c[1].dir) == "/sdcard/Android/data/com.readyatdawn.r15/files");
  CHECK(std::string(c[1].trusted) == "/sdcard/Android/data");
  for (std::size_t i = 0; i < n; ++i) {  // every folder is below its trusted part
    const std::string dir = c[i].dir;
    const std::string trusted = c[i].trusted;
    CHECK(dir.compare(0, trusted.size() + 1, trusted + "/") == 0);
  }
}

// The folder line is not lost when the write that carried it failed, and a log at its size bound that
// the folder line fills still takes the record that came with it.
void TheFolderLineSurvivesAFailedWriteAndTheBound() {
  RemoveTree(Dir() + "/an");
  g_fakeNow = 840000;
  Cands c;
  c.Add("only", Dir() + "/an/files", Dir());
  c.Install();
  RefuseAllWrites();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "refused first");
  AllowWrites();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "accepted next");
  const std::string disk = Slurp(Dir() + "/an/files/nevr-sentinel.log");
  const std::size_t folder = disk.find("\"event\":\"sentinel_log_dir\"");
  const std::size_t next = disk.find("accepted next");
  CHECK(folder != std::string::npos && next != std::string::npos && folder < next);
  CHECK(disk.find("\"event\":\"sentinel_log_dir\"", folder + 1) == std::string::npos);  // once

  // A log 20 bytes under the bound: the folder line fills it, the record is not dropped.
  RemoveTree(Dir() + "/an");
  g_fakeNow = 850000;
  c.Install();  // forgets the chosen folder
  CHECK(::mkdir((Dir() + "/an").c_str(), 0755) == 0);
  CHECK(::mkdir((Dir() + "/an/files").c_str(), 0755) == 0);
  WriteFile(Dir() + "/an/files/nevr-sentinel.log", std::string(static_cast<std::size_t>(sentinel::kMaxDiskLogBytes) - 20, 'q'));
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "record after the folder line");
  CHECK(Slurp(Dir() + "/an/files/nevr-sentinel.log").find("record after the folder line") != std::string::npos);

  // Two queued lines (a failure, then the winner) with the log 20 bytes under its bound: the first fills it and the
  // second, and the record, still land.
  RemoveTree(Dir() + "/an");
  g_fakeNow = 860000;
  WriteFile(Dir() + "/blocker2", "a file");
  Cands two;
  two.Add("blocked", Dir() + "/blocker2/files", Dir());
  two.Add("good", Dir() + "/an/files", Dir());
  two.Install();
  CHECK(::mkdir((Dir() + "/an").c_str(), 0755) == 0);
  CHECK(::mkdir((Dir() + "/an/files").c_str(), 0755) == 0);
  WriteFile(Dir() + "/an/files/nevr-sentinel.log", std::string(static_cast<std::size_t>(sentinel::kMaxDiskLogBytes) - 20, 'q'));
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "record after two queued lines");
  const std::string fresh = Slurp(Dir() + "/an/files/nevr-sentinel.log");
  CHECK(fresh.find("\"event\":\"sentinel_log_dir\"") != std::string::npos);
  CHECK(fresh.find("record after two queued lines") != std::string::npos);
  ::unlink((Dir() + "/blocker2").c_str());
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/an");
}

// Each time the folder is chosen the lines are news again and reach the disk, however many times that has happened
// (the queue of lines waiting for the disk empties and is reused).
void EveryChoiceOfTheFolderIsReportedAndWritten() {
  RemoveTree(Dir() + "/rc");
  g_fakeNow = 870000;
  WriteFile(Dir() + "/blocker3", "a file");
  Cands c;
  c.Add("blocked", Dir() + "/blocker3/files", Dir());
  c.Add("good", Dir() + "/rc/files", Dir());
  c.Install();
  Captured().clear();
  for (int cycle = 1; cycle <= 4; ++cycle) {
    sentinel::Emit(nevr_quest::LogLevel::kInfo, "cycle " + std::to_string(cycle));
    const std::string disk = Slurp(Dir() + "/rc/files/nevr-sentinel.log");
    CHECK(disk.find("\"event\":\"sentinel_log_dir_failed\"") != std::string::npos);
    CHECK(disk.find("\"event\":\"sentinel_log_dir\"") != std::string::npos);
    CHECK(Events("sentinel_log_dir_failed").size() == static_cast<std::size_t>(cycle));
    CHECK(Events("sentinel_log_dir").size() == static_cast<std::size_t>(cycle));
    RemoveTree(Dir() + "/rc");  // the user cleared the app's files
    sentinel::CloseDiskLog();
    sentinel::Emit(nevr_quest::LogLevel::kInfo, "gone " + std::to_string(cycle));  // ENOENT: choose again at the next try
    g_fakeNow += sentinel::kOpenRetryMs + 1;
  }
  ::unlink((Dir() + "/blocker3").c_str());
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/rc");
}

// ---- the crash folder (#1797): crash_dir.h, the one chooser behind the writer and the sender ----

std::vector<std::string> UnusableLines() {
  std::vector<std::string> lines;
  for (const Line& l : Captured()) {
    if (l.text.find("crash dir candidate unusable") != std::string::npos) lines.push_back(l.text);
  }
  return lines;
}

void TheShippedCrashFoldersAreMediaFirst() {
  const sentinel::LogDirCandidate* c = sentinel::crash_dir::kShipped;
  const std::size_t n = std::size(sentinel::crash_dir::kShipped);
  CHECK(n == 4);
  if (n != 4) return;
  CHECK(std::string(c[0].name) == "android_media" &&
        std::string(c[0].dir) == std::string(nevr_quest::data_root::kMediaRoot) + "/nevr-crashes");
  CHECK(std::string(c[0].trusted) == "/sdcard/Android/media/com.readyatdawn.r15");
  CHECK(std::string(c[1].name) == "android_data" &&
        std::string(c[1].dir) == "/sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes");
  CHECK(std::string(c[1].trusted) == "/sdcard/Android/data");
  CHECK(std::string(c[2].name) == "app_internal" && std::string(c[2].dir) == "/data/data/com.readyatdawn.r15/files/nevr-crashes");
  CHECK(std::string(c[2].trusted) == "/data/data/com.readyatdawn.r15");
  CHECK(std::string(c[3].name) == "local_tmp" && std::string(c[3].dir) == "/data/local/tmp/nevr-crashes");
  CHECK(std::string(c[3].trusted) == "/data/local/tmp");
  for (std::size_t i = 0; i < n; ++i) {  // every folder is below its trusted part
    const std::string dir = c[i].dir;
    const std::string trusted = c[i].trusted;
    CHECK(dir.compare(0, trusted.size() + 1, trusted + "/") == 0);
  }
}

void TheCrashFolderIsMadeBelowTheTrustedPart() {
  RemoveTree(Dir() + "/cr");
  Captured().clear();
  Cands c;
  c.Add("media", Dir() + "/cr/a/b/nevr-crashes", Dir());  // cr, a and b do not exist
  c.Add("data", Dir() + "/cr/other/nevr-crashes", Dir());
  c.Install();
  const std::string chosen = sentinel::crash_dir::ChooseFrom(c.list.data(), c.list.size());
  CHECK(chosen == Dir() + "/cr/a/b/nevr-crashes");
  CHECK(IsDir(Dir() + "/cr/a/b/nevr-crashes"));
  CHECK(!IsDir(Dir() + "/cr/other"));  // the first usable candidate wins; the rest are not made
  CHECK(UnusableLines().empty());
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/cr");
}

void ACrashFolderThatCannotBeUsedFallsThroughAndIsNamed() {
  RemoveTree(Dir() + "/cf");
  CHECK(::mkdir((Dir() + "/cf").c_str(), 0755) == 0);
  WriteFile(Dir() + "/cf/blocker", "a file");
  Captured().clear();
  Cands c;
  c.Add("media", Dir() + "/cf/blocker/nevr-crashes", Dir() + "/cf");       // a file in the way: ENOTDIR
  c.Add("data", Dir() + "/cf/data/files/nevr-crashes", Dir() + "/cf");
  c.Install();
  const std::string chosen = sentinel::crash_dir::ChooseFrom(c.list.data(), c.list.size());
  CHECK(chosen == Dir() + "/cf/data/files/nevr-crashes");
  const std::vector<std::string> unusable = UnusableLines();
  CHECK(unusable.size() == 1);
  if (unusable.size() == 1) {
    // The line names the candidate, its folder and the errno.
    CHECK(unusable[0].find("candidate=media") != std::string::npos);
    CHECK(unusable[0].find("dir=" + Dir() + "/cf/blocker/nevr-crashes") != std::string::npos);
    CHECK(unusable[0].find("errno=" + std::to_string(ENOTDIR) + " (" + std::strerror(ENOTDIR) + ")") != std::string::npos);
  }
  CHECK(Captured().front().prio == 5);  // ANDROID_LOG_WARN

  // A folder that exists but cannot be written into (root writes anywhere, so this part needs another user).
  if (::geteuid() != 0) {
    CHECK(::mkdir((Dir() + "/cf/ro").c_str(), 0700) == 0);
    CHECK(::mkdir((Dir() + "/cf/ro/nevr-crashes").c_str(), 0500) == 0);
    Captured().clear();
    Cands r;
    r.Add("media", Dir() + "/cf/ro/nevr-crashes", Dir() + "/cf");
    r.Add("data", Dir() + "/cf/rw/nevr-crashes", Dir() + "/cf");
    r.Install();
    CHECK(sentinel::crash_dir::ChooseFrom(r.list.data(), r.list.size()) == Dir() + "/cf/rw/nevr-crashes");
    const std::vector<std::string> ro = UnusableLines();
    CHECK(ro.size() == 1 && ro[0].find("errno=" + std::to_string(EACCES)) != std::string::npos);
  }

  // Nothing usable: "" (the caller falls back to "."), every candidate named.
  Captured().clear();
  Cands none;
  none.Add("media", Dir() + "/cf/blocker/a", Dir() + "/cf");
  none.Add("data", Dir() + "/cf/blocker/b", Dir() + "/cf");
  none.Install();
  CHECK(sentinel::crash_dir::ChooseFrom(none.list.data(), none.list.size()).empty());
  CHECK(UnusableLines().size() == 2);
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/cf");
}

void TheBuildsOwnCrashFolderIsTriedFirstAndNeverCreated() {
  RemoveTree(Dir() + "/ov");
  CHECK(::mkdir((Dir() + "/ov").c_str(), 0755) == 0);
  CHECK(sentinel::crash_dir::Choose((Dir() + "/ov").c_str()) == Dir() + "/ov");  // exists and writable: wins over every shipped folder
  // A build folder that is missing is not made: the choice moves on to the shipped list. On a host none of those can be made.
  Captured().clear();
  CHECK(sentinel::crash_dir::Choose((Dir() + "/ov/missing/deeper").c_str()) != Dir() + "/ov/missing/deeper");
  CHECK(!IsDir(Dir() + "/ov/missing"));
  CHECK(UnusableLines().size() >= 1 && UnusableLines()[0].find("candidate=override") != std::string::npos);
  // The build's folder missing: the list that follows is used, and the override is named as the one that failed.
  RemoveTree(Dir() + "/ov");
  CHECK(::mkdir((Dir() + "/ov").c_str(), 0755) == 0);
  Cands list;
  list.Add("media", Dir() + "/ov/list/nevr-crashes", Dir() + "/ov");
  list.Install();
  Captured().clear();
  CHECK(sentinel::crash_dir::Choose((Dir() + "/ov/nope/deeper").c_str(), list.list.data(), list.list.size()) ==
        Dir() + "/ov/list/nevr-crashes");
  CHECK(UnusableLines().size() == 1 && UnusableLines()[0].find("candidate=override") != std::string::npos);
  // A usable build folder wins over a usable list; an empty one is no override at all, and says nothing.
  CHECK(sentinel::crash_dir::Choose(Dir().c_str(), list.list.data(), list.list.size()) == Dir());
  Captured().clear();
  CHECK(sentinel::crash_dir::Choose("", list.list.data(), list.list.size()) == Dir() + "/ov/list/nevr-crashes");
  CHECK(UnusableLines().empty());
  CHECK(sentinel::crash_dir::Choose(nullptr, list.list.data(), list.list.size()) == Dir() + "/ov/list/nevr-crashes");
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  RemoveTree(Dir() + "/ov");
}

void TheBuiltInListIsTheOverrideAloneInATestBuild() {
  sentinel::SetLogDirCandidatesForTest(nullptr, 0);
  CHECK(sentinel::LogDir() == Dir());
  CHECK(std::string(sentinel::FilesDir()) == Dir());
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
  ALeftoverJsonConfigIsReportedAndNeverRead();
  NoLogLineCarriesAValue();
  DiskLogIsOneJsonObjectPerLine();
  FormatEscapesHostileMessages();
  StructuredDiskLogIsFlat();
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
  RotationFailureIsThrottled();
  RetryIntervalIgnoresTheWallClock();
  UnopenablePathsAreRetriedAfterTheWindow();
  TheBuiltInListIsTheOverrideAloneInATestBuild();
  TheShippedCandidatesAreMediaThenData();
  TheShippedCrashFoldersAreMediaFirst();
  TheCrashFolderIsMadeBelowTheTrustedPart();
  ACrashFolderThatCannotBeUsedFallsThroughAndIsNamed();
  TheBuildsOwnCrashFolderIsTriedFirstAndNeverCreated();
  TheFolderLineSurvivesAFailedWriteAndTheBound();
  EveryChoiceOfTheFolderIsReportedAndWritten();
  TheLogFolderChainIsCreatedAndReportedOnce();
  ALinkBelowTheTrustedPartIsNotFollowed();
  AFirstCandidateThatCannotBeUsedFallsThrough();
  TheFolderIsChosenAgainWhenItVanishes();
  ClearLogs();
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

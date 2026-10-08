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
                                "file.example", "stub-emb.example"};

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

void DiskLogAtAFifoNeverBlocks() {
  sentinel::CloseDiskLog();
  ::mkdir(Dir().c_str(), 0755);
  ::unlink((Dir() + "/nevr-sentinel.log").c_str());
  const std::string fifo = Dir() + "/nevr-sentinel.log";
  CHECK(::mkfifo(fifo.c_str(), 0600) == 0);

  // No reader: the non-blocking open fails with ENXIO instead of blocking.
  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "fifo line one");
  CHECK(Count("fifo line one") == 1);

  // A reader present: the open succeeds, the type check refuses it, and it is not retried.
  sentinel::CloseDiskLog();
  const int reader = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
  CHECK(reader >= 0);
  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "fifo line two");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "fifo line three");
  CHECK(Count("fifo line two") == 1 && Count("fifo line three") == 1);
  CHECK(Count("on-disk log open failed: path is not a regular file") == 1);
  char buf[64];
  CHECK(::read(reader, buf, sizeof(buf)) <= 0);  // nothing was written into the FIFO
  ::close(reader);
  ::unlink(fifo.c_str());
  sentinel::CloseDiskLog();
}

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
}

void LargeLogIsRotatedNotDeleted() {
  sentinel::CloseDiskLog();
  ::mkdir(Dir().c_str(), 0755);
  const std::string log = Dir() + "/nevr-sentinel.log";
  WriteFile(log, std::string(static_cast<std::size_t>(sentinel::kMaxDiskLogBytes) + 1, 'x'));
  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "after rotation");
  CHECK(ReadAll(log).size() < 1024);
  CHECK(ReadAll(log).find("after rotation") != std::string::npos);
  int rotated = 0;
  std::size_t rotatedBytes = 0;
  for (const std::string& name : ListDir()) {
    if (name.rfind("nevr-sentinel.", 0) == 0 && name != "nevr-sentinel.log") {
      ++rotated;
      rotatedBytes = ReadAll(Dir() + "/" + name).size();
      ::unlink((Dir() + "/" + name).c_str());
    }
  }
  CHECK(rotated == 1);
  CHECK(rotatedBytes == static_cast<std::size_t>(sentinel::kMaxDiskLogBytes) + 1);
  sentinel::CloseDiskLog();
  ::unlink(log.c_str());
}

void EachDiskFailureClassIsReportedOnce() {
  sentinel::CloseDiskLog();
  ::unlink((Dir() + "/nevr-sentinel.log").c_str());
  ::unlink((Dir() + "/nevr-quest.json").c_str());
  CHECK(::rmdir(Dir().c_str()) == 0);

  Captured().clear();
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "first line");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "second line");
  CHECK(Count("on-disk log open failed") == 1);
  CHECK(Count("first line") == 1 && Count("second line") == 1);

  CHECK(::mkdir(Dir().c_str(), 0755) == 0);
  // A regular file that refuses every write: the file-size limit is zero (EFBIG once SIGXFSZ is ignored).
  std::signal(SIGXFSZ, SIG_IGN);
  struct rlimit saved {};
  CHECK(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
  struct rlimit zero = saved;
  zero.rlim_cur = 0;
  CHECK(::setrlimit(RLIMIT_FSIZE, &zero) == 0);
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "third line");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "fourth line");
  CHECK(::setrlimit(RLIMIT_FSIZE, &saved) == 0);
  CHECK(Count("on-disk log write failed") == 1);
  CHECK(Count("on-disk log open failed") == 1);
  CHECK(Count("third line") == 1 && Count("fourth line") == 1);
  sentinel::CloseDiskLog();
  ::unlink((Dir() + "/nevr-sentinel.log").c_str());
  ::rmdir(Dir().c_str());
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
  TornRecordsStayOnTheirOwnLine();
  LargeLogIsRotatedNotDeleted();
  EachDiskFailureClassIsReportedOnce();
  DiskLogAtAFifoNeverBlocks();
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

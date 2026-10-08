// Host test for the sentinel's configuration activation and logging (activation.cpp,
// sentinel_log.cpp) against a stand-in for liblog. Link order matters on purpose: this file comes
// first, so its constructor runs before activation.cpp's static initializers, the same order as
// entry.cpp's constructor relative to the sentinel sources.
//
// Compile with -DNEVR_QUEST_FILES_DIR="<dir>" (a scratch directory this test creates and removes).
#include "activation.h"
#include "sentinel_log.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <fcntl.h>
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
                                "STUB-EMBEDDED-API-SECRET-5521", "STUB-EMBEDDED-SERVER-SECRET-9973"};

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

// Mirrors entry.cpp: the ELF constructor resolves the configuration before main.
__attribute__((constructor)) void HostCtor() {
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

void NoLogLineCarriesAValue() {
  CHECK(!Captured().empty());
  for (const Line& l : Captured()) {
    for (const char* secret : kSecrets) CHECK(l.text.find(secret) == std::string::npos);
    CHECK(l.text.find("file.example") == std::string::npos);
    CHECK(l.text.find("stub-emb.example") == std::string::npos);
  }
  const std::string disk = ReadAll(Dir() + "/nevr-sentinel.log");
  CHECK(!disk.empty());
  for (const char* secret : kSecrets) CHECK(disk.find(secret) == std::string::npos);
  CHECK(disk.find("file.example") == std::string::npos);
  CHECK(disk.find("FILE-SECRET") == std::string::npos);
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
  CHECK(::symlink("/dev/full", (Dir() + "/nevr-sentinel.log").c_str()) == 0);
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "third line");
  sentinel::Emit(nevr_quest::LogLevel::kInfo, "fourth line");
  CHECK(Count("on-disk log write failed") == 1);
  CHECK(Count("on-disk log open failed") == 1);
  CHECK(Count("third line") == 1 && Count("fourth line") == 1);
  ::unlink((Dir() + "/nevr-sentinel.log").c_str());
  ::rmdir(Dir().c_str());
}

}  // namespace

extern "C" {

int __android_log_write(int prio, const char*, const char* text) {
  Captured().push_back({prio, text});
  return 1;
}

int __android_log_print(int prio, const char*, const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Captured().push_back({prio, buf});
  return 1;
}

}  // extern "C"

int main() {
  alarm(30);  // a regression that blocks on a FIFO must fail, not hang
  ConfigSurvivesStaticInitialisation();
  NoLogLineCarriesAValue();
  DiskLogIsOneJsonObjectPerLine();
  FormatEscapesHostileMessages();
  ReadConfigFileNeverBlocksOnNonRegularFiles();
  WriteAllRetriesAndReportsFailure();
  EachDiskFailureClassIsReportedOnce();
  if (g_failures != 0) {
    std::fprintf(stderr, "sentinel_host_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("sentinel_host_test: all checks pass\n");
  return 0;
}

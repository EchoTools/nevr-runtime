// Log() knows the boot phase (#593). While the runtime boots under the DllMain loader lock, the
// game's CLog is not usable (#495), so Log() reports through the boot tee; once the tee closes, it
// reports through CLog. These tests link the real Log() (src/core/logging.cpp) and the real tee
// (src/runtime/log/boot_log_tee.cpp) and stand a recording function in for CLog.

#include <gtest/gtest.h>
#include <winsock2.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <vector>

#include "core/logging.h"
#include "runtime/lifecycle/crash_record_line.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/log/run_log_writer.h"

namespace {

int g_clogCalls = 0;
std::string g_clogLast;

VOID RecordingClog(EchoVR::LogLevel, UINT64, const CHAR* format, va_list args) {
  char buf[512];
  vsnprintf(buf, sizeof(buf), format, args);
  ++g_clogCalls;
  g_clogLast = buf;
}

// Points EchoVR::WriteLog (what InitializeFunctionPointers resolves to the game's CLog) at the recorder
// for one test, as it is during boot after the function pointers are resolved.
struct RecordingClogScope {
  EchoVR::WriteLogFunc* saved;
  RecordingClogScope() : saved(EchoVR::WriteLog) {
    EchoVR::WriteLog = &RecordingClog;
    g_clogCalls = 0;
    g_clogLast.clear();
  }
  ~RecordingClogScope() { EchoVR::WriteLog = saved; }
};

std::streamoff FileSize(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  return in ? static_cast<std::streamoff>(in.tellg()) : 0;
}

std::string ReadFrom(const std::string& path, std::streamoff start) {
  std::ifstream in(path, std::ios::binary);
  in.seekg(start);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Runs `fn` with the process's stderr (fd 2) pointed at a scratch file and returns what it wrote.
template <typename Fn>
std::string CaptureStderr(Fn fn) {
  char dir[MAX_PATH];
  char path[MAX_PATH];
  GetTempPathA(sizeof(dir), dir);
  GetTempFileNameA(dir, "tee", 0, path);
  std::fflush(stderr);
  const int saved = _dup(2);
  const int fd = _open(path, _O_WRONLY | _O_TRUNC | _O_BINARY);
  _dup2(fd, 2);
  _close(fd);
  fn();
  std::fflush(stderr);
  _dup2(saved, 2);
  _close(saved);
  const std::string out = ReadFrom(path, 0);
  DeleteFileA(path);
  return out;
}

// The boot phase writes into the process's one main log, <root>\logs\nevr-<creation time>.jsonl (docs/standards/logging.md, "One log"),
// never a file beside the game: each test gives the process its own %LOCALAPPDATA%.
class BootLogRouting : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ = GetEnv();
    root_ = std::filesystem::temp_directory_path() /
            ("nevr-boot-routing-" + std::to_string(GetCurrentProcessId()) + "-" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root_.wstring().c_str());
    nevr_run_log_writer::Close();
    exe_boot_file_ = ExeDirBootFile();
    exe_boot_size_ = FileSize(exe_boot_file_);
  }
  void TearDown() override {
    nevr_run_log_writer::Close();
    SetEnvironmentVariableW(L"LOCALAPPDATA", saved_.empty() ? nullptr : saved_.c_str());
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  std::filesystem::path LogsDir() const { return root_ / "nevr" / "echovr" / "logs"; }
  std::vector<nlohmann::json> Records(const std::string& path) const {
    std::vector<nlohmann::json> records;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
      EXPECT_TRUE(record.is_object()) << "not a JSON record: " << line;
      records.push_back(std::move(record));
    }
    return records;
  }

  std::filesystem::path root_;
  std::string exe_boot_file_;
  std::streamoff exe_boot_size_ = 0;
  std::wstring saved_;

 private:
  static std::wstring GetEnv() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    return n == 0 || n >= sizeof(buf) / sizeof(buf[0]) ? std::wstring() : std::wstring(buf, n);
  }
  // Where the old boot file was: logs\nevr-boot.jsonl beside the executable.
  static std::string ExeDirBootFile() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string path(exe);
    path.resize(path.find_last_of("\\/"));
    return path + "\\logs\\nevr-boot.jsonl";
  }
};

}  // namespace

// #653: the first stderr line the tee writes ends in a newline, so the next stderr line starts on its own.
TEST_F(BootLogRouting, TheOpeningLineEndsInANewlineOnStderr) {
  const std::string err = CaptureStderr([] { nevr_boot_log_tee::Init(); });
  nevr_boot_log_tee::Close();
  const std::string::size_type at = err.find("[NEVR.PATCH] info; boot log opened run=");
  ASSERT_NE(at, std::string::npos) << "stderr was:\n" << err;
  const std::string::size_type end = err.find('\n', at);
  ASSERT_NE(end, std::string::npos) << "the opening line has no newline; stderr was:\n" << err;
  const std::string::size_type next = err.find("[NEVR", at + 1);
  if (next != std::string::npos) {
    EXPECT_EQ(err[next - 1], '\n') << "the next line runs onto the opening line:\n" << err;
  }
}

TEST_F(BootLogRouting, ALogCallDuringBootReachesTheTeeNotClog) {
  RecordingClogScope clog;
  nevr_boot_log_tee::Init();
  const std::string path = nevr_boot_log_tee::Path();
  ASSERT_FALSE(path.empty()) << "the boot tee could not resolve the main log";
  const std::streamoff start = FileSize(path);

  Log(EchoVR::LogLevel::Warning, "[NEVR.HEADLESS] boot routing probe id=%d", 593);

  const std::string written = ReadFrom(path, start);
  nevr_boot_log_tee::Close();
  EXPECT_EQ(g_clogCalls, 0) << "Log() during boot entered the game's CLog with: " << g_clogLast;
  // Rule 12: a boot line carries its level after the subsystem tag.
  EXPECT_NE(written.find("[NEVR.HEADLESS] warn; boot routing probe id=593"), std::string::npos)
      << "boot log was:\n" << written;
}

// #2245: during boot a Log() level with no name is written "unknown" with its raw value, never as info; error and warn
// keep their names.
TEST_F(BootLogRouting, ALevelWithNoNameIsUnknownWithItsRawValueAndErrorStaysError) {
  RecordingClogScope clog;
  nevr_boot_log_tee::Init();
  const std::string path = nevr_boot_log_tee::Path();
  ASSERT_FALSE(path.empty()) << "the boot tee could not resolve the main log";
  const std::streamoff start = FileSize(path);

  Log(static_cast<EchoVR::LogLevel>(3), "[NEVR.HEADLESS] odd level probe id=%d", 2245);
  Log(EchoVR::LogLevel::Error, "[NEVR.HEADLESS] error level probe id=%d", 2245);

  const std::string written = ReadFrom(path, start);
  nevr_boot_log_tee::Close();
  std::vector<nlohmann::json> records;
  for (std::string::size_type at = 0, nl; (nl = written.find('\n', at)) != std::string::npos; at = nl + 1) {
    records.push_back(nlohmann::json::parse(written.substr(at, nl - at)));
  }
  ASSERT_EQ(records.size(), 2U) << "boot log was:\n" << written;
  EXPECT_EQ(records[0].at("level"), "unknown");
  EXPECT_EQ(records[0].at("msg"), "[NEVR.HEADLESS] unknown; level_raw=3 odd level probe id=2245\n");
  EXPECT_EQ(records[1].at("level"), "error");
  EXPECT_EQ(records[1].at("msg"), "[NEVR.HEADLESS] error; error level probe id=2245\n");
}

TEST_F(BootLogRouting, ALogCallAfterBootReachesClogNotTheTee) {
  RecordingClogScope clog;
  nevr_boot_log_tee::Init();
  const std::string path = nevr_boot_log_tee::Path();
  ASSERT_FALSE(path.empty()) << "the boot tee could not resolve the main log";
  nevr_boot_log_tee::Close();
  const std::streamoff start = FileSize(path);

  Log(EchoVR::LogLevel::Info, "[NEVR.HEADLESS] post-boot routing probe id=%d", 593);

  EXPECT_EQ(g_clogCalls, 1);
  EXPECT_EQ(g_clogLast, "[NEVR.HEADLESS] post-boot routing probe id=593");
  EXPECT_EQ(ReadFrom(path, start), "") << "Log() after boot still wrote to the boot file";
}

// One log (docs/standards/logging.md, "One log"): the boot phase's lines are records of the main log, with src "boot", and no
// separate boot file is written beside the game.
TEST_F(BootLogRouting, BootLinesAreRecordsOfTheMainLogAndNoBootFileIsWrittenBesideTheGame) {
  nevr_boot_log_tee::Init();
  Log(EchoVR::LogLevel::Warning, "[NEVR.HEADLESS] one log probe id=%d", 2041);
  nevr_boot_log_tee::Close();

  const std::string path = nevr_run_log_writer::Path();
  ASSERT_FALSE(path.empty());
  const std::string name = std::filesystem::path(path).filename().string();
  EXPECT_EQ(std::filesystem::path(path).parent_path(), LogsDir());
  EXPECT_EQ(name.rfind("nevr-", 0), 0u) << name;
  EXPECT_EQ(name.substr(name.size() - 6), ".jsonl") << name;

  const std::vector<nlohmann::json> records = Records(path);
  bool found = false;
  for (const nlohmann::json& record : records) {
    if (record.value("msg", "").find("one log probe id=2041") == std::string::npos) continue;
    found = true;
    EXPECT_EQ(record.value("src", ""), "boot");
    EXPECT_EQ(record.value("level", ""), "warn");
    EXPECT_EQ(record.value("run", ""), std::string(GetRunId()));
    EXPECT_TRUE(record.contains("ts"));
  }
  EXPECT_TRUE(found) << "the probe line is not in the main log";
  EXPECT_EQ(FileSize(exe_boot_file_), exe_boot_size_) << "the tee still wrote logs\\nevr-boot.jsonl beside the game";
}

// Where the main log cannot be made there is no other place: the failure is on stderr, the lines are counted as lost and nothing is
// written beside the game.
TEST_F(BootLogRouting, WithNoRootThereIsNoFileAnywhereAndTheLossIsCounted) {
  SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  nevr_run_log_writer::Close();
  const std::string err = CaptureStderr([] {
    nevr_boot_log_tee::Init();
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] no root probe");
    nevr_boot_log_tee::Close();
  });
  EXPECT_STREQ(nevr_run_log_writer::Path(), "");
  EXPECT_GE(nevr_run_log_writer::LostRecords(), 2u);
  EXPECT_NE(err.find("[NEVR.LOG] error;"), std::string::npos) << "the loss is not silent; stderr was:\n" << err;
  EXPECT_EQ(FileSize(exe_boot_file_), exe_boot_size_);
}

// The filter writes through the same object, so its records and the boot phase's are one stream in one file.
TEST_F(BootLogRouting, TheFiltersWriterAndTheTeeShareOneFile) {
  nevr_boot_log_tee::Init();
  const std::string line = "{\"ts\":\"2026-11-04T00:00:00.000Z\",\"run\":\"r\",\"level\":\"info\",\"msg\":\"filter probe\"}\n";
  ASSERT_TRUE(nevr_run_log_writer::Append(line.data(), line.size()));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] tee probe");
  nevr_boot_log_tee::Close();

  const std::vector<nlohmann::json> records = Records(nevr_run_log_writer::Path());
  bool filter = false, tee = false;
  for (const nlohmann::json& record : records) {
    filter = filter || record.value("msg", "") == "filter probe";
    tee = tee || record.value("msg", "").find("tee probe") != std::string::npos;
  }
  EXPECT_TRUE(filter && tee);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(LogsDir()), std::filesystem::directory_iterator()), 1) << "one file";
}

// Rotation keeps the process's file name: the old content is renamed away, so the host and the crash path, which append to that name,
// keep writing to the live file.
TEST_F(BootLogRouting, RotationRenamesTheOldContentAndKeepsTheProcessesFileName) {
  const std::string first = "{\"msg\":\"before\"}\n";
  const std::string second = "{\"msg\":\"after\"}\n";
  ASSERT_TRUE(nevr_run_log_writer::Append(first.data(), first.size()));
  const std::string path = nevr_run_log_writer::Path();
  ASSERT_TRUE(nevr_run_log_writer::Rotate());
  ASSERT_TRUE(nevr_run_log_writer::Append(second.data(), second.size()));

  EXPECT_EQ(nevr_run_log_writer::Path(), path) << "the name did not change";
  EXPECT_EQ(ReadFrom(path, 0), second);
  std::string archive = path;
  archive.resize(archive.size() - 6);
  archive += ".1.jsonl";
  EXPECT_EQ(ReadFrom(archive, 0), first);
}

// The crash path writes each [NEVR.CRASH] line as one record of the main log (crash_record_line.h), built in a fixed buffer.
TEST(CrashRecordLine, ACrashLineIsOneJsonRecordOfTheMainLog) {
  char out[1536];
  const std::string text = "[NEVR.CRASH] exception code=0xC0000005 \"quoted\"\tTab\n";
  const std::size_t n = nevr_crash_recovery::BuildCrashRecord(text.data(), text.size(), "2026-11-04T01:02:03.004Z", "1a-2b", out, sizeof(out));
  ASSERT_NE(n, 0u);
  ASSERT_EQ(out[n - 1], '\n');
  EXPECT_EQ(std::string(out, n).find('\n'), n - 1) << "one record is one line";
  const nlohmann::json record = nlohmann::json::parse(std::string(out, n - 1), nullptr, false);
  ASSERT_TRUE(record.is_object()) << std::string(out, n);
  EXPECT_EQ(record.value("tag", ""), "NEVR.CRASH");
  EXPECT_EQ(record.value("event", ""), "crash_record");
  EXPECT_EQ(record.value("level", ""), "error");
  EXPECT_EQ(record.value("run", ""), "1a-2b");
  EXPECT_EQ(record.value("ts", ""), "2026-11-04T01:02:03.004Z");
  EXPECT_EQ(record.value("msg", ""), "[NEVR.CRASH] exception code=0xC0000005 \"quoted\"\tTab");
}

TEST(CrashRecordLine, ARecordThatDoesNotFitIsRefusedNotCut) {
  char tiny[64];
  EXPECT_EQ(nevr_crash_recovery::BuildCrashRecord("x", 1, "2026-11-04T01:02:03.004Z", "r", tiny, sizeof(tiny)), 0u);
}

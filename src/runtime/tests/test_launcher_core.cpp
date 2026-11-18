// The helpers of echovr_server.exe (launcher/launcher_core.h): the command-line quoting the game parses back, the logs
// folder listing and the follower that shows one run's records. The quoting is held to the Windows parser itself:
// every argument must come back from CommandLineToArgvW as it went in.

#include <gtest/gtest.h>

#include <climits>
#include <cstdio>
#include <string>
#include <vector>

#include "launcher/launcher_core.h"

#include <shellapi.h>

namespace {

constexpr unsigned long long kNoLogIsRecent = ULLONG_MAX;
constexpr unsigned long long kAnyTime = 0;
constexpr const char* kRunId = "1a2b-ff";
constexpr size_t kArgsInSeries = 4;
constexpr size_t kRegionValueIndex = 3;
constexpr size_t kTwoRecords = 2;
constexpr size_t kTwoNotices = 2;
constexpr int kTwoFlushes = 2;
constexpr const char* kFirstLog = "nevr-2026-10-30T12-00-01.000.jsonl";
constexpr const char* kSecondLog = "nevr-2026-10-30T12-00-02.000.jsonl";

// What CommandLineToArgvW makes of `command`, argument 0 (the program name) dropped.
std::vector<std::string> ParseBack(const std::string& command) {
  const std::string full = "prog.exe " + command;
  std::wstring wide(full.begin(), full.end());
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(wide.c_str(), &argc);
  std::vector<std::string> out;
  if (argv == nullptr) return out;
  for (int i = 1; i < argc; ++i) {
    const std::wstring w = argv[i];
    out.emplace_back(w.begin(), w.end());
  }
  LocalFree(argv);
  return out;
}

std::string RoundTrip(const std::string& arg) {
  const std::vector<std::string> back = ParseBack(nevr_launcher::QuoteArgForWindows(arg));
  return back.size() == 1 ? back[0] : std::string("<") + std::to_string(back.size()) + " arguments>";
}

TEST(LauncherQuote, PlainArgumentsAreLeftAlone) {
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("-config-file"), "-config-file");
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("C:\\echo\\config.yaml"), "C:\\echo\\config.yaml");
}

TEST(LauncherQuote, EmptyAndSpacedArgumentsAreQuoted) {
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows(""), "\"\"");
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("a b"), "\"a b\"");
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("a\tb"), "\"a\tb\"");
}

TEST(LauncherQuote, QuotesAndBackslashesBeforeThemAreEscaped) {
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("say \"hi\""), "\"say \\\"hi\\\"\"");
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("a\\\"b"), "\"a\\\\\\\"b\"");
}

TEST(LauncherQuote, TrailingBackslashesAreDoubledSoTheClosingQuoteSurvives) {
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("C:\\Program Files\\"), "\"C:\\Program Files\\\\\"");
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("a b\\\\"), "\"a b\\\\\\\\\"");
}

TEST(LauncherQuote, BackslashesInTheMiddleAreKept) {
  EXPECT_EQ(nevr_launcher::QuoteArgForWindows("C:\\Program Files\\x"), "\"C:\\Program Files\\x\"");
}

TEST(LauncherQuote, EveryArgumentComesBackFromTheWindowsParserAsItWentIn) {
  const std::vector<std::string> args = {"-config-file", "", "C:\\Program Files\\Echo VR\\config.yaml", "a\"b", "a\\\"b",
                                         "trailing\\", "two words\\\\", "tab\tsplit", "\\\\server\\share dir\\", "\"", "\\"};
  for (const std::string& arg : args) {
    EXPECT_EQ(RoundTrip(arg), arg) << "argument: " << arg;
  }
}

TEST(LauncherQuote, ASeriesOfArgumentsStaysSeparate) {
  std::string line;
  for (const char* arg : {"-config-file", "C:\\a b\\c.yaml", "-region", ""}) line += " " + nevr_launcher::QuoteArgForWindows(arg);
  const std::vector<std::string> back = ParseBack(line);
  ASSERT_EQ(back.size(), kArgsInSeries);
  EXPECT_EQ(back[1], "C:\\a b\\c.yaml");
  EXPECT_EQ(back[kRegionValueIndex], "");
}

TEST(LauncherDirectory, IsTheFullPathWithoutItsLastComponent) {
  EXPECT_EQ(nevr_launcher::DirectoryOfPath("C:\\Echo VR\\bin\\echovr_server.exe"), "C:\\Echo VR\\bin");
  EXPECT_EQ(nevr_launcher::DirectoryOfPath("C:\\echovr_server.exe"), "C:");
  EXPECT_EQ(nevr_launcher::DirectoryOfPath("echovr_server.exe"), "");
  EXPECT_EQ(nevr_launcher::DirectoryOfPath(""), "");
}

// A fresh logs folder under the temp directory, removed with its files.
class LauncherLogsFolder : public ::testing::Test {
 protected:
  void SetUp() override {
    char temp[MAX_PATH] = {};
    ASSERT_GT(GetTempPathA(sizeof(temp), temp), 0u);
    dir_ = std::string(temp) + "nevr_launcher_core_" + std::to_string(GetCurrentProcessId()) + "_" +
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    ASSERT_TRUE(CreateDirectoryA(dir_.c_str(), nullptr)) << dir_;
  }
  void TearDown() override {
    for (const std::string& name : created_) DeleteFileA((dir_ + "\\" + name).c_str());
    RemoveDirectoryA((dir_ + "\\sub.jsonl").c_str());
    RemoveDirectoryA(dir_.c_str());
  }
  void Write(const std::string& name, const std::string& text, const char* mode = "wb") {
    std::FILE* f = std::fopen((dir_ + "\\" + name).c_str(), mode);
    ASSERT_NE(f, nullptr);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    created_.push_back(name);
  }
  static std::string Record(const char* run, const char* level, const char* msg) {
    return std::string("{\"ts\":\"2026-10-30T12:00:01.000Z\",\"run\":\"") + run + "\",\"level\":\"" + level + "\",\"msg\":\"" +
           msg + "\"}\n";
  }
  std::string dir_;
  std::vector<std::string> created_;
};

TEST_F(LauncherLogsFolder, ListsEveryJsonlFileWithItsWriteTime) {
  Write(kFirstLog, "x\n");
  Write("other.txt", "x\n");
  ASSERT_TRUE(CreateDirectoryA((dir_ + "\\sub.jsonl").c_str(), nullptr));
  const std::vector<nevr_server_log::LogFileInfo> files = nevr_launcher::ListLogFiles(dir_);
  ASSERT_EQ(files.size(), 1u);  // not the .txt, not the folder that ends in .jsonl
  EXPECT_EQ(files[0].name, kFirstLog);
  EXPECT_GT(files[0].written, 0ULL);
}

TEST_F(LauncherLogsFolder, AnAbsentFolderListsNothing) {
  EXPECT_TRUE(nevr_launcher::ListLogFiles(dir_ + "\\missing").empty());
}

struct Captured {
  std::vector<std::string> lines;
  std::vector<std::string> notices;
  int flushes = 0;
  nevr_launcher::FollowerSink Sink() {
    nevr_launcher::FollowerSink sink;
    sink.line = [this](const std::string& s) { lines.push_back(s); };
    sink.notice = [this](const std::string& s) { notices.push_back(s); };
    sink.flush = [this] { ++flushes; };
    return sink;
  }
};

TEST_F(LauncherLogsFolder, FollowerShowsThisRunsRecordsAndAnnouncesTheFileOnce) {
  Write(kFirstLog, Record(kRunId, "info", "first") + Record("9999-1", "info", "another run") + Record(kRunId, "debug", "hidden"));
  Captured out;
  nevr_launcher::RunLogFollower follower(dir_, kRunId, kAnyTime, out.Sink());
  follower.Poll();
  ASSERT_EQ(out.lines.size(), 1u);
  EXPECT_EQ(out.lines[0], "2026-10-30T12:00:01.000Z info first");
  ASSERT_EQ(out.notices.size(), 1u);
  EXPECT_EQ(out.notices[0], "[echovr_server] Log file: " + dir_ + "\\" + kFirstLog);

  Write(kFirstLog, Record(kRunId, "warn", "second"), "ab");
  follower.Poll();
  ASSERT_EQ(out.lines.size(), kTwoRecords);  // only what the file gained
  EXPECT_EQ(out.lines[1], "2026-10-30T12:00:01.000Z warn second");
  EXPECT_EQ(out.notices.size(), 1u);  // announced once
}

TEST_F(LauncherLogsFolder, FollowerFlushesAfterEachFilesLines) {
  Write(kFirstLog, Record(kRunId, "info", "one"));
  Write(kSecondLog, Record(kRunId, "info", "two"));
  Captured out;
  nevr_launcher::RunLogFollower follower(dir_, kRunId, kAnyTime, out.Sink());
  follower.Poll();
  EXPECT_EQ(out.flushes, kTwoFlushes);  // one per followed file
}

TEST_F(LauncherLogsFolder, TheConsoleSinkPutsRecordsOnStdoutAndNoticesOnStderr) {
  Write(kFirstLog, Record(kRunId, "info", "console record"));
  nevr_launcher::RunLogFollower follower(dir_, kRunId, kAnyTime);  // the default sink
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  follower.Poll();
  const std::string out = testing::internal::GetCapturedStdout();
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(out, "2026-10-30T12:00:01.000Z info console record\n");
  EXPECT_EQ(err, "[echovr_server] Log file: " + dir_ + "\\" + kFirstLog + "\n");
}

TEST_F(LauncherLogsFolder, FollowerFollowsARotatedFileOfTheSameRun) {
  Write(kFirstLog, Record(kRunId, "info", "before rotation"));
  Captured out;
  nevr_launcher::RunLogFollower follower(dir_, kRunId, kAnyTime, out.Sink());
  follower.Poll();
  Write(kSecondLog, Record(kRunId, "info", "after rotation"));
  follower.Poll();
  ASSERT_EQ(out.lines.size(), kTwoRecords);
  EXPECT_EQ(out.lines[1], "2026-10-30T12:00:01.000Z info after rotation");
  EXPECT_EQ(out.notices.size(), kTwoNotices);
}

TEST_F(LauncherLogsFolder, FollowerLeavesFilesWrittenBeforeTheStartAlone) {
  Write(kFirstLog, Record(kRunId, "info", "old"));
  Captured out;
  nevr_launcher::RunLogFollower follower(dir_, kRunId, kNoLogIsRecent, out.Sink());
  follower.Poll();
  EXPECT_TRUE(out.lines.empty());
  EXPECT_TRUE(out.notices.empty());
}

TEST_F(LauncherLogsFolder, FollowerNamesAnUnreadableFileOnceAndShowsNothingOfIt) {
  Write(kFirstLog, Record(kRunId, "info", "locked"));
  const std::string path = dir_ + "\\" + kFirstLog;
  // A share mode of 0 keeps every other open out, as a process that holds the log exclusively would.
  HANDLE lock = CreateFileA(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(lock, INVALID_HANDLE_VALUE);
  Captured out;
  nevr_launcher::RunLogFollower follower(dir_, kRunId, kAnyTime, out.Sink());
  follower.Poll();
  follower.Poll();
  CloseHandle(lock);
  EXPECT_TRUE(out.lines.empty());
  ASSERT_EQ(out.notices.size(), 1u);  // reported once, not on every poll
  EXPECT_NE(out.notices[0].find("is not readable (errno: "), std::string::npos) << out.notices[0];
  EXPECT_NE(out.notices[0].find(path), std::string::npos);
}

}  // namespace

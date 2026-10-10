// #5: the boot log's records and their replay into the main log.

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/json_escape.h"
#include "runtime/log/boot_lines.h"
#include "runtime/log/boot_replay.h"

namespace {

std::string Record(const char* ts, const char* run, const std::string& rawMsg) {
  char escaped[512];
  JsonEscape::Into(rawMsg.data(), static_cast<int>(rawMsg.size()), escaped, sizeof(escaped));
  char line[1024];
  const int n = nevr_boot_lines::Build(line, sizeof(line), ts, run, escaped);
  EXPECT_GT(n, 0);
  return std::string(line, static_cast<size_t>(n));
}

TEST(BootLines, RecordIsOneJsonLineWithATimestamp) {
  const std::string line = Record("2026-10-09T01:02:03.004Z", "run-1", "hooks \"ok\"\n");
  ASSERT_EQ(line.back(), '\n');
  const auto json = nlohmann::json::parse(line);
  EXPECT_EQ(json.at("ts"), "2026-10-09T01:02:03.004Z");
  EXPECT_EQ(json.at("run"), "run-1");
  EXPECT_EQ(json.at("level"), "info");
  EXPECT_EQ(json.at("msg"), "hooks \"ok\"\n");
}

TEST(BootLines, BuildReportsATooSmallBuffer) {
  char tiny[16];
  EXPECT_EQ(nevr_boot_lines::Build(tiny, sizeof(tiny), "2026-10-09T01:02:03.004Z", "run-1", "x"), -1);
}

TEST(BootReplay, ReplaysOnlyThisRunInFileOrder) {
  const std::string file = Record("2026-10-09T01:00:00.000Z", "old-run", "from an earlier run") +
                           Record("2026-10-09T02:00:00.001Z", "run-2", "first") +
                           Record("2026-10-09T01:30:00.000Z", "other", "someone else") +
                           Record("2026-10-09T02:00:00.002Z", "run-2", "second \x1b[0m ctl");
  const auto lines = nevr_boot_replay::ParseRun(file, "run-2");
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_EQ(lines[0].ts, "2026-10-09T02:00:00.001Z");
  EXPECT_EQ(lines[0].msg, "first");
  EXPECT_EQ(lines[0].level, "info");
  EXPECT_EQ(lines[1].msg, "second \x1b[0m ctl") << "escapes round-trip";
}

// The boot tee keeps writing after the main log opens. The tail replay reads from the byte offset
// where the first stopped, so every complete line lands exactly once, in file order, however much
// the file has grown, and a line another thread is still writing waits for the next call.
class BootReplayFile : public ::testing::Test {
 protected:
  void SetUp() override {
    char dir[MAX_PATH];
    ASSERT_GT(GetTempPathA(MAX_PATH, dir), 0U);
    path_ = std::string(dir) + "boot_replay_test_" + std::to_string(GetCurrentProcessId()) + ".jsonl";
    std::remove(path_.c_str());
  }
  void TearDown() override { std::remove(path_.c_str()); }
  void Append(const std::string& text) {
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    out << text;
  }
  std::vector<std::string> Next() {
    std::vector<nevr_boot_replay::Line> lines;
    int err = 0;
    EXPECT_TRUE(nevr_boot_replay::ReadNew(path_.c_str(), "run-2", cursor_, lines, &err)) << "errno " << err;
    std::vector<std::string> msgs;
    for (const auto& line : lines) msgs.push_back(line.msg);
    return msgs;
  }
  std::string path_;
  nevr_boot_replay::Cursor cursor_;
};

TEST_F(BootReplayFile, TheTailAddsEachLaterLineOnceInOrderAndWaitsForAHalfWrittenOne) {
  Append(Record("2026-10-09T02:00:00.001Z", "run-2", "one") + Record("2026-10-09T01:00:00.000Z", "other", "x") +
         Record("2026-10-09T02:00:00.002Z", "run-2", "two"));
  EXPECT_EQ(Next(), (std::vector<std::string>{"one", "two"}));
  const std::string half = Record("2026-10-09T02:00:00.005Z", "run-2", "five");
  Append(Record("2026-10-09T02:00:00.003Z", "run-2", "three") + Record("2026-10-09T02:00:00.004Z", "run-2", "four") +
         half.substr(0, half.size() / 2));
  EXPECT_EQ(Next(), (std::vector<std::string>{"three", "four"})) << "the half-written line is not consumed";
  Append(half.substr(half.size() / 2) + Record("2026-10-09T02:00:00.006Z", "run-2", "six"));
  EXPECT_EQ(Next(), (std::vector<std::string>{"five", "six"})) << "completed, and exactly once";
  EXPECT_TRUE(Next().empty());
}

TEST_F(BootReplayFile, AFileThatGrowsPastTheFirstWindowBetweenCallsLosesNothing) {
  Append(Record("2026-10-09T02:00:00.001Z", "run-2", "before"));
  EXPECT_EQ(Next(), (std::vector<std::string>{"before"}));
  std::string filler;
  const std::string noise = Record("2026-10-09T01:00:00.000Z", "other-run", std::string(900, 'x'));
  Append(Record("2026-10-09T02:00:00.002Z", "run-2", "just after"));
  while (filler.size() < 2U * 1024U * 1024U) filler += noise;
  Append(filler);
  Append(Record("2026-10-09T02:00:00.003Z", "run-2", "last"));
  EXPECT_EQ(Next(), (std::vector<std::string>{"just after", "last"}));
}

TEST_F(BootReplayFile, TheFirstCallReadsOnlyTheLastMiBAndDropsItsPartialFirstLine) {
  const std::string noise = Record("2026-10-09T01:00:00.000Z", "other-run", std::string(900, 'x'));
  std::string filler;
  while (filler.size() < 2U * 1024U * 1024U) filler += noise;
  Append(Record("2026-10-09T02:00:00.001Z", "run-2", "too old for the window") + filler +
         Record("2026-10-09T02:00:00.002Z", "run-2", "recent"));
  EXPECT_EQ(Next(), (std::vector<std::string>{"recent"}));
}

TEST_F(BootReplayFile, AnUnreadableFileIsReported) {
  std::vector<nevr_boot_replay::Line> lines;
  int err = 0;
  EXPECT_FALSE(nevr_boot_replay::ReadNew(path_.c_str(), "run-2", cursor_, lines, &err));
  EXPECT_NE(err, 0);
}

TEST(BootReplay, SkipsLinesItCannotPlace) {
  const std::string file = std::string("not json\n") + "[1,2,3]\n" + "\n" +
                           "{\"run\":\"run-2\",\"level\":\"info\",\"msg\":\"no ts\"}\n" +
                           "{\"ts\":\"2026-10-09T02:00:00.000Z\",\"run\":\"run-2\",\"level\":\"info\"}\n" +
                           "{\"ts\":5,\"run\":\"run-2\",\"msg\":\"ts not a string\"}\n" +
                           Record("2026-10-09T02:00:00.003Z", "run-2", "kept") + "{\"truncated\":";
  const auto lines = nevr_boot_replay::ParseRun(file, "run-2");
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0].msg, "kept");
  EXPECT_TRUE(nevr_boot_replay::ParseRun("", "run-2").empty());
}

}  // namespace

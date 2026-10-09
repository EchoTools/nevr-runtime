// #5: the boot log's records and their replay into the main log.

#include <gtest/gtest.h>

#include <string>

#include "core/json_escape.h"
#include "runtime/log/boot_lines.h"
#include "runtime/log/boot_replay.h"

namespace {

std::string Record(const char* ts, const char* run, const std::string& rawMsg) {
  char escaped[512];
  JsonEscape::Into(rawMsg.data(), static_cast<int>(rawMsg.size()), escaped, sizeof(escaped));
  char line[1024];
  const int n = BootLines::Build(line, sizeof(line), ts, run, escaped);
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
  EXPECT_EQ(BootLines::Build(tiny, sizeof(tiny), "2026-10-09T01:02:03.004Z", "run-1", "x"), -1);
}

TEST(BootReplay, ReplaysOnlyThisRunInFileOrder) {
  const std::string file = Record("2026-10-09T01:00:00.000Z", "old-run", "from an earlier run") +
                           Record("2026-10-09T02:00:00.001Z", "run-2", "first") +
                           Record("2026-10-09T01:30:00.000Z", "other", "someone else") +
                           Record("2026-10-09T02:00:00.002Z", "run-2", "second \x1b[0m ctl");
  const auto lines = BootReplay::ParseRun(file, "run-2");
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_EQ(lines[0].ts, "2026-10-09T02:00:00.001Z");
  EXPECT_EQ(lines[0].msg, "first");
  EXPECT_EQ(lines[0].level, "info");
  EXPECT_EQ(lines[1].msg, "second \x1b[0m ctl") << "escapes round-trip";
}

TEST(BootReplay, SkipsLinesItCannotPlace) {
  const std::string file = std::string("not json\n") + "[1,2,3]\n" + "\n" +
                           "{\"run\":\"run-2\",\"level\":\"info\",\"msg\":\"no ts\"}\n" +
                           "{\"ts\":\"2026-10-09T02:00:00.000Z\",\"run\":\"run-2\",\"level\":\"info\"}\n" +
                           "{\"ts\":5,\"run\":\"run-2\",\"msg\":\"ts not a string\"}\n" +
                           Record("2026-10-09T02:00:00.003Z", "run-2", "kept") + "{\"truncated\":";
  const auto lines = BootReplay::ParseRun(file, "run-2");
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0].msg, "kept");
  EXPECT_TRUE(BootReplay::ParseRun("", "run-2").empty());
}

}  // namespace

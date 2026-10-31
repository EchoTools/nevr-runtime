// #1246: the server launcher's run log: which files are run logs, which of them are this run's, and what one record shows.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/run_id.h"
#include "launcher/server_log.h"
#include "runtime/log/boot_replay.h"

namespace {

using nevr_server_log::LogFileInfo;

TEST(ServerLogName, RunLogsAreTheTimestampedNevrJsonlFiles) {
  EXPECT_TRUE(nevr_server_log::IsRunLogName("nevr-2026-10-30T12-00-00.123.jsonl"));
  EXPECT_FALSE(nevr_server_log::IsRunLogName("nevr-boot.jsonl"));       // the boot spool
  EXPECT_FALSE(nevr_server_log::IsRunLogName("nevr-bootstrap.jsonl"));  // the host's log
  EXPECT_FALSE(nevr_server_log::IsRunLogName("nevr-2026-10-30.log"));   // the text form is not the launcher's
  EXPECT_FALSE(nevr_server_log::IsRunLogName("other-2026-10-30T12-00-00.123.jsonl"));
  EXPECT_FALSE(nevr_server_log::IsRunLogName("nevr-.jsonl"));
}

TEST(ServerLogCandidates, OnlyRecentRunLogsInStartOrder) {
  const unsigned long long since = 1000;
  const std::vector<LogFileInfo> files = {
      {"nevr-2026-10-30T12-00-02.000.jsonl", 2000},  // newer run log
      {"nevr-2026-10-30T12-00-01.000.jsonl", 1500},  // this run's first file
      {"nevr-2026-10-29T09-00-00.000.jsonl", 500},   // a previous day's run: written before the start
      {"nevr-bootstrap.jsonl", 3000},                // the host's log, recent but not a run log
      {"nevr-boot.jsonl", 3000},                     // the boot spool, recent but not a run log
  };
  const std::vector<std::string> got = nevr_server_log::RunLogCandidates(files, since);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], "nevr-2026-10-30T12-00-01.000.jsonl");
  EXPECT_EQ(got[1], "nevr-2026-10-30T12-00-02.000.jsonl");
}

TEST(ServerLogRecords, OnlyThisRunsRecordsAreShown) {
  const std::string contents =
      "{\"ts\":\"2026-10-30T12:00:01.000Z\",\"run\":\"1a2b-ff\",\"level\":\"info\",\"msg\":\"[NEVR.WS] login success xpid=OVR-ORG-1\"}\n"
      "{\"ts\":\"2026-10-30T12:00:01.500Z\",\"run\":\"9999-1\",\"level\":\"info\",\"msg\":\"another run\"}\n"
      "{\"ts\":\"2026-10-30T12:00:02.000Z\",\"run\":\"1a2b-ff\",\"level\":\"debug\",\"msg\":\"per-frame detail\"}\n"
      "{\"ts\":\"2026-10-30T12:00:03.000Z\",\"run\":\"1a2b-ff\",\"level\":\"warn\",\"msg\":\"line one\\nline two\"}\n";
  std::vector<std::string> shown;
  for (const nevr_boot_replay::Line& line : nevr_boot_replay::ParseRun(contents, "1a2b-ff")) {
    const std::string text = nevr_server_log::RenderLine(line);
    if (!text.empty()) shown.push_back(text);
  }
  ASSERT_EQ(shown.size(), 2u);
  EXPECT_EQ(shown[0], "2026-10-30T12:00:01.000Z info [NEVR.WS] login success xpid=OVR-ORG-1");
  EXPECT_EQ(shown[1], "2026-10-30T12:00:03.000Z warn line one line two");
}

TEST(ServerLogRecords, DebugIsNotShown) {
  nevr_boot_replay::Line line;
  line.ts = "2026-10-30T12:00:02.000Z";
  line.level = "debug";
  line.msg = "per-frame detail";
  EXPECT_EQ(nevr_server_log::RenderLine(line), "");
}

TEST(RunId, FormatIsPidAndCreationTimeInHex) {
  char buf[40] = {};
  nevr_run_id::Format(0x1a2bUL, 0x01d8f0c2b3e4f5a6ULL, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1a2b-1d8f0c2b3e4f5a6");
}

TEST(RunId, FormatNeverWritesPastItsBuffer) {
  char buf[4] = {};
  nevr_run_id::Format(0x1a2bUL, 0x01d8f0c2b3e4f5a6ULL, buf, sizeof(buf));
  EXPECT_EQ(std::string(buf).size(), 3u);
}

}  // namespace

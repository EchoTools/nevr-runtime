// #1296: the built-in filter keeps every run log while the logs folder is under its size cap, and deletes the oldest
// only past it. The open log is never deleted. These tests drive the function the filter calls (log/log_retention.h).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/log/log_retention.h"

namespace {

using nevr_log_retention::RunLogFile;
using nevr_log_retention::SelectLogsToDelete;

constexpr uint64_t kLogBytes = 100;

std::vector<RunLogFile> SevenLogs() {
  std::vector<RunLogFile> files;
  for (int i = 1; i <= 7; ++i) {
    RunLogFile f;
    f.path = "logs\\nevr-2026-11-0" + std::to_string(i) + "T10-00-00.jsonl";
    f.bytes = kLogBytes;
    files.push_back(f);
  }
  return files;
}

TEST(LogRetention, SevenRunLogsUnderTheCapAllStay) {
  // The falsifier of #1296: 7 run logs, the folder under its cap, nothing is deleted.
  const auto doomed = SelectLogsToDelete(SevenLogs(), 7 * kLogBytes, "logs\\nevr-2026-11-07T10-00-00.jsonl");
  EXPECT_TRUE(doomed.empty());
}

TEST(LogRetention, NoCapKeepsEverything) {
  // A server's default cap is 0: no run log is ever deleted, however large the folder grows.
  const auto doomed = SelectLogsToDelete(SevenLogs(), 0, "logs\\nevr-2026-11-07T10-00-00.jsonl");
  EXPECT_TRUE(doomed.empty());
}

TEST(LogRetention, PastTheCapTheOldestAreDeletedUntilUnder) {
  // 7 logs of 100 bytes against a cap of 450: the total goes 700, 600, 500, 400. The three oldest are deleted, and
  // the selection stops at the first total at or under the cap.
  const auto doomed = SelectLogsToDelete(SevenLogs(), 450, "logs\\nevr-2026-11-07T10-00-00.jsonl");
  ASSERT_EQ(doomed.size(), 3u);
  EXPECT_EQ(doomed[0], "logs\\nevr-2026-11-01T10-00-00.jsonl");
  EXPECT_EQ(doomed[1], "logs\\nevr-2026-11-02T10-00-00.jsonl");
  EXPECT_EQ(doomed[2], "logs\\nevr-2026-11-03T10-00-00.jsonl");
}

TEST(LogRetention, TheOpenLogIsNeverDeletedEvenWhenItIsTheOldest) {
  // The open file sorts first here, so it is the oldest by name. It counts toward the total, is skipped, and the
  // next oldest goes instead.
  const std::string open = "logs\\nevr-2026-11-01T10-00-00.jsonl";
  const auto doomed = SelectLogsToDelete(SevenLogs(), 450, open);
  for (const std::string& path : doomed) EXPECT_NE(path, open);
  ASSERT_EQ(doomed.size(), 3u);
  EXPECT_EQ(doomed[0], "logs\\nevr-2026-11-02T10-00-00.jsonl");
}

TEST(LogRetention, AnOpenLogAloneOverTheCapDeletesNothing) {
  std::vector<RunLogFile> one;
  RunLogFile f;
  f.path = "logs\\nevr-2026-11-01T10-00-00.jsonl";
  f.bytes = 5000;
  one.push_back(f);
  EXPECT_TRUE(SelectLogsToDelete(one, 100, f.path).empty());
}

TEST(LogRetention, OrderIsByNameNotByListOrder) {
  // The folder lists files in no fixed order; the oldest by name is deleted first.
  auto files = SevenLogs();
  std::reverse(files.begin(), files.end());
  const auto doomed = SelectLogsToDelete(files, 450, "logs\\nevr-2026-11-07T10-00-00.jsonl");
  ASSERT_EQ(doomed.size(), 3u);
  EXPECT_EQ(doomed[0], "logs\\nevr-2026-11-01T10-00-00.jsonl");
}

}  // namespace

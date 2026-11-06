// #2245: a record of the main log carries the level its line was given. The built-in filter writes every game line and
// every runtime Log() line through nevr_run_log_record::Build (log/builtin_filter.cpp, WriteFileRecord); these tests
// drive that function and parse what it writes.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/log/run_log_record.h"

namespace {

nlohmann::json Record(std::uint32_t gameLevel, bool insideErrorDialog, const char* message) {
  std::string line;
  nevr_run_log_record::Build(line, "2026-11-06T12:00:00.000Z", "run-2245", gameLevel, insideErrorDialog, message,
                             static_cast<int>(std::strlen(message)));
  EXPECT_FALSE(line.empty());
  EXPECT_EQ(line.back(), '\n');
  return nlohmann::json::parse(line);
}

}  // namespace

// echovr.exe 0x14072bf97: "Failed to initialize OVR library: %s" is sent at level 8.
TEST(RunLogRecord, AGameLineSentAtErrorIsAnError) {
  const nlohmann::json r = Record(8, false, "[EVR] Failed to initialize OVR library: -3001");
  EXPECT_EQ(r.at("level"), "error");
  EXPECT_FALSE(r.contains("level_raw"));
  EXPECT_FALSE(r.contains("game_level"));
}

TEST(RunLogRecord, AGameLineSentAtWarningIsAWarn) {
  EXPECT_EQ(Record(4, false, "[EVR] a warning").at("level"), "warn");
}

TEST(RunLogRecord, DebugAndInfoKeepTheirNames) {
  EXPECT_EQ(Record(1, false, "[EVR] detail").at("level"), "debug");
  EXPECT_EQ(Record(2, false, "[EVR] note").at("level"), "info");
}

// A level the mapping has no name for is not written as info: it is "unknown", and the number it came with is kept.
TEST(RunLogRecord, AnUnmappableLevelIsUnknownWithItsRawValue) {
  for (const std::uint32_t raw : {0u, 3u, 0xEu, 0xFu, 16u, 1000u}) {
    const nlohmann::json r = Record(raw, false, "[EVR] odd level");
    EXPECT_EQ(r.at("level"), "unknown") << "raw " << raw;
    ASSERT_TRUE(r.contains("level_raw")) << "raw " << raw;
    EXPECT_EQ(r.at("level_raw"), raw);
  }
}

// echovr.exe 0x1400ea730, the error dialog, logs its message at level 2 before it shows the box; the line is an error,
// and the record keeps the level the game gave it.
TEST(RunLogRecord, ALineTheGameLogsFromItsErrorDialogIsAnError) {
  const nlohmann::json r = Record(2, true, "[EVR] Unknown error while loading the game.");
  EXPECT_EQ(r.at("level"), "error");
  ASSERT_TRUE(r.contains("game_level"));
  EXPECT_EQ(r.at("game_level"), 2);
  EXPECT_EQ(r.at("msg"), "[EVR] Unknown error while loading the game.");
}

TEST(RunLogRecord, TheHeaderKeysAndTheMessageAreKept) {
  const nlohmann::json r = Record(2, false, "quote \" and backslash \\");
  EXPECT_EQ(r.at("ts"), "2026-11-06T12:00:00.000Z");
  EXPECT_EQ(r.at("run"), "run-2245");
  EXPECT_EQ(r.at("msg"), "quote \" and backslash \\");
}

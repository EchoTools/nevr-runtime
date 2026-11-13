// Load timing records (runtime/hook/load_timing_rules.h): the level_load record, the boot_timing record and the LoadLevel wrapper,
// with fake clocks. The same header is compiled for the Quest by src/quest/tests/load_timing_rules_test.cpp.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "runtime/hook/load_timing_rules.h"

namespace {

struct FakeClock {
  std::uint64_t mono = 1000;
  std::uint64_t epoch = 1700000000000ULL;
  std::uint64_t MonotonicMs() { return mono; }
  std::uint64_t EpochMs() { return epoch; }
};

TEST(LoadTiming, LevelLoadRecordCarriesLevelHashMsAndStart) {
  EXPECT_EQ(nevr_load_timing::FormatLevelLoad("mnu_master", 0x1234ABCDULL, 335, 1700000000123ULL, true),
            "[NEVR.BOOT] level_load level=mnu_master level_hash=0x1234abcd ms=335 start_ts_ms=1700000000123 first_load=true");
}

TEST(LoadTiming, UnnamedLevelIsUnknownButKeepsItsHash) {
  EXPECT_EQ(nevr_load_timing::FormatLevelLoad(nullptr, 0xFFULL, 7, 5, false),
            "[NEVR.BOOT] level_load level=unknown level_hash=0xff ms=7 start_ts_ms=5 first_load=false");
}

TEST(LoadTiming, BootTimingRecordIsWrittenOnceAtLoggedIn) {
  nevr_load_timing::BootTimer timer;
  std::string line;
  timer.MarkEngineReady(4200);
  EXPECT_TRUE(timer.MarkLoggedIn(9100, &line));
  EXPECT_EQ(line, "[NEVR.BOOT] boot_timing engine_ready_ms=4200 logged_in_ms=9100");
  line.clear();
  EXPECT_FALSE(timer.MarkLoggedIn(20000, &line));  // a second login (reconnect) writes nothing
  EXPECT_TRUE(line.empty());
}

TEST(LoadTiming, FirstEngineReadyMarkWins) {
  nevr_load_timing::BootTimer timer;
  std::string line;
  timer.MarkEngineReady(4200);
  timer.MarkEngineReady(8000);
  ASSERT_TRUE(timer.MarkLoggedIn(9100, &line));
  EXPECT_EQ(line, "[NEVR.BOOT] boot_timing engine_ready_ms=4200 logged_in_ms=9100");
}

TEST(LoadTiming, EngineReadyNeverSeenPrintsMinusOne) {
  nevr_load_timing::BootTimer timer;
  std::string line;
  ASSERT_TRUE(timer.MarkLoggedIn(9100, &line));
  EXPECT_EQ(line, "[NEVR.BOOT] boot_timing engine_ready_ms=-1 logged_in_ms=9100");
}

TEST(LoadTiming, EngineReadyLineIsTheGamesFinishedInitializingEngine) {
  EXPECT_TRUE(nevr_load_timing::IsEngineReadyLine("[INFO] Finished initializing engine"));
  EXPECT_FALSE(nevr_load_timing::IsEngineReadyLine("[LEVELLOAD] Finished loading level 'mnu_master' in 335 ms"));
  EXPECT_FALSE(nevr_load_timing::IsEngineReadyLine(nullptr));
}

TEST(LoadTiming, ALongLevelNameIsCarriedWholeAndEveryFieldSurvives) {
  const std::string longName(400, 'x');
  const std::string line = nevr_load_timing::FormatLevelLoad(longName.c_str(), 0xABULL, 12, 99, true);
  EXPECT_NE(line.find(" level=" + longName + " level_hash=0xab ms=12 start_ts_ms=99 first_load=true"), std::string::npos) << line;
}

TEST(LoadTiming, ElapsedIsTheDifferenceOfTwoReadableTimes) {
  EXPECT_EQ(nevr_load_timing::ElapsedMs(5000, 1000), 4000);
}

TEST(LoadTiming, ElapsedIsUnknownNotZeroWhenTheStartCannotBeRead) {
  EXPECT_EQ(nevr_load_timing::ElapsedMs(5000, 0), nevr_load_timing::kUnknownMsValue);
  EXPECT_EQ(nevr_load_timing::ElapsedMs(1000, 5000), nevr_load_timing::kUnknownMsValue);
}

TEST(LoadTiming, AnUnknownMarkIsRecordedAsMinusOneNotAsAMeasurement) {
  nevr_load_timing::BootTimer timer;
  std::string line;
  timer.MarkEngineReady(nevr_load_timing::kUnknownMsValue);
  ASSERT_TRUE(timer.MarkLoggedIn(nevr_load_timing::kUnknownMsValue, &line));
  EXPECT_EQ(line, "[NEVR.BOOT] boot_timing engine_ready_ms=-1 logged_in_ms=-1");
}

TEST(LoadTiming, HookNotArmedWarningCarriesAddressExpectedAndActualBytes) {
  const std::uint8_t expected[] = {0x48, 0x8B, 0xC4, 0x48};
  const std::uint8_t actual[] = {0xE9, 0x01, 0x02, 0x03};
  EXPECT_EQ(nevr_load_timing::FormatHookNotArmed("LoadLevel", "prologue_mismatch", 0x1404FE050ULL, expected, actual, 4),
            "[NEVR.BOOT] load timing not armed name=LoadLevel va=0x1404FE050 reason=prologue_mismatch "
            "expected=488bc448 actual=e9010203 (no detour was installed; level loads are not timed)");
  EXPECT_EQ(nevr_load_timing::FormatHookNotArmed("LoadLevel", "detour_failed", 0x10, expected, nullptr, 4),
            "[NEVR.BOOT] load timing not armed name=LoadLevel va=0x10 reason=detour_failed expected=488bc448 "
            "actual=unreadable (no detour was installed; level loads are not timed)");
}

TEST(LoadTiming, WrapperTimesTheCallAndPassesTheResultThrough) {
  FakeClock clock;
  std::vector<std::string> records;
  nevr_load_timing::FirstLoad first;
  const std::uint64_t result = nevr_load_timing::TimeLevelLoad(
      clock, [&](const std::string& line) { records.push_back(line); }, first, 0xABULL, "mpl_arena_a", [&] {
        clock.mono += 250;  // the load takes 250 ms
        return std::uint64_t{77};
      });
  EXPECT_EQ(result, 77u);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0],
            "[NEVR.BOOT] level_load level=mpl_arena_a level_hash=0xab ms=250 start_ts_ms=1700000000000 first_load=true");
}

TEST(LoadTiming, WrapperWritesOneRecordPerLoad) {
  FakeClock clock;
  std::vector<std::string> records;
  auto sink = [&](const std::string& line) { records.push_back(line); };
  nevr_load_timing::FirstLoad first;
  for (int i = 0; i < 3; ++i) {
    nevr_load_timing::TimeLevelLoad(clock, sink, first, 1, "a", [&] { clock.mono += 10; return 0; });
  }
  ASSERT_EQ(records.size(), 3u);
  EXPECT_NE(records[0].find(" first_load=true"), std::string::npos);
  EXPECT_NE(records[1].find(" first_load=false"), std::string::npos);
  EXPECT_NE(records[2].find(" first_load=false"), std::string::npos);
}

TEST(LoadTiming, ClockGoingBackwardsIsMinusOneNotAMeasurement) {
  FakeClock clock;
  std::vector<std::string> records;
  nevr_load_timing::FirstLoad first;
  nevr_load_timing::TimeLevelLoad(clock, [&](const std::string& l) { records.push_back(l); }, first, 1, "a",
                                  [&] { clock.mono -= 5; return 0; });
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], "[NEVR.BOOT] level_load level=a level_hash=0x1 ms=-1 start_ts_ms=1700000000000 first_load=true");
}

TEST(LoadTiming, UnreadableStartTimeIsMinusOneNotZero) {
  FakeClock clock;
  clock.epoch = 0;
  std::vector<std::string> records;
  nevr_load_timing::FirstLoad first;
  nevr_load_timing::TimeLevelLoad(clock, [&](const std::string& l) { records.push_back(l); }, first, 1, "a",
                                  [&] { clock.mono += 5; return 0; });
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], "[NEVR.BOOT] level_load level=a level_hash=0x1 ms=5 start_ts_ms=-1 first_load=true");
}

TEST(LoadTiming, WrapperWritesOneRecordForAVoidCall) {
  FakeClock clock;
  std::vector<std::string> records;
  nevr_load_timing::FirstLoad first;
  bool ran = false;
  nevr_load_timing::TimeLevelLoad(clock, [&](const std::string& l) { records.push_back(l); }, first, 2, "v",
                                  [&] { ran = true; clock.mono += 9; });
  EXPECT_TRUE(ran);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0], "[NEVR.BOOT] level_load level=v level_hash=0x2 ms=9 start_ts_ms=1700000000000 first_load=true");
}

TEST(LoadTiming, EngineReadyAfterLoggedInIsIgnored) {
  nevr_load_timing::BootTimer timer;
  std::string line;
  ASSERT_TRUE(timer.MarkLoggedIn(9100, &line));
  timer.MarkEngineReady(9500);
  EXPECT_EQ(line, "[NEVR.BOOT] boot_timing engine_ready_ms=-1 logged_in_ms=9100");
  EXPECT_FALSE(timer.MarkLoggedIn(9600, &line));
}

TEST(LoadTiming, ConcurrentLoggedInMarksWriteExactlyOneRecord) {
  for (int round = 0; round < 50; ++round) {
    nevr_load_timing::BootTimer timer;
    std::atomic<int> written{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
      threads.emplace_back([&, i] {
        timer.MarkEngineReady(100 + i);
        std::string line;
        if (timer.MarkLoggedIn(1000 + i, &line)) written.fetch_add(1);
      });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(written.load(), 1);
  }
}

TEST(LoadTiming, ConcurrentFirstLoadTakesAreTrueOnce) {
  for (int round = 0; round < 50; ++round) {
    nevr_load_timing::FirstLoad first;
    std::atomic<int> trues{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) threads.emplace_back([&] { if (first.Take()) trues.fetch_add(1); });
    for (auto& t : threads) t.join();
    EXPECT_EQ(trues.load(), 1);
  }
}

}  // namespace

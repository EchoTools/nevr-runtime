// #2099: the per-player stats as the game's /session writer reads them. A fake net game record holds the three words of each
// entry (type, value, count) at the writer's offsets, 0x30 apart, per slot at STATS_STRIDE. The reader returns the value, or
// value / count for an average (type 5). Plain checks, no gtest, so this runs on a host without it.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/server/telemetry_snapshot.h"

namespace {

int g_failures = 0;

#define EXPECT_EQ_U32(actual, expected)                                                                   \
  do {                                                                                                    \
    const uint32_t a_ = (actual);                                                                         \
    const uint32_t e_ = (expected);                                                                       \
    if (a_ != e_) {                                                                                       \
      std::printf("FAIL %s:%d: %s = %u, want %u\n", __FILE__, __LINE__, #actual, a_, e_);              \
      ++g_failures;                                                                                       \
    }                                                                                                     \
  } while (0)

void PutWord(std::vector<char>& rec, uint64_t at, uint32_t word) { std::memcpy(rec.data() + at, &word, sizeof word); }

void PutFloat(std::vector<char>& rec, uint64_t at, float value) { std::memcpy(rec.data() + at, &value, sizeof value); }

// Slot 0 and slot 1 each get a full record; the writer's stats sit inside the first STATS_STRIDE bytes of a slot's record.
std::vector<char> NetGame() { return std::vector<char>(nevr_game_offsets::STATS_STRIDE * 2 + 0x8000, 0); }

void TestReadsEachStatAtTheWritersOffset() {
  using namespace nevr_game_offsets;
  std::vector<char> rec = NetGame();
  const uint64_t slot1 = STATS_STRIDE;  // slot 1's record
  PutWord(rec, slot1 + STAT_GOALS + STAT_WORD_TYPE, 1);
  PutWord(rec, slot1 + STAT_GOALS + STAT_WORD_VALUE, 3);
  PutWord(rec, slot1 + STAT_GOALS + STAT_WORD_COUNT, 0);
  PutWord(rec, slot1 + STAT_STUNS + STAT_WORD_TYPE, 1);
  PutWord(rec, slot1 + STAT_STUNS + STAT_WORD_VALUE, 7);
  PutWord(rec, slot1 + STAT_STUNS + STAT_WORD_COUNT, 0);
  const char* net = rec.data();
  EXPECT_EQ_U32(ReadStatValue(net, 1, STAT_GOALS), 3u);
  EXPECT_EQ_U32(ReadStatValue(net, 1, STAT_STUNS), 7u);
  // The old 0x10-stride offsets are not stats: a value planted at the old goals slot 0x10 is not read as goals.
  PutWord(rec, slot1 + 0x10 + STAT_WORD_VALUE, 99);
  EXPECT_EQ_U32(ReadStatValue(net, 1, STAT_GOALS), 3u);
}

void TestAverageEntryDividesValueByCount() {
  using namespace nevr_game_offsets;
  std::vector<char> rec = NetGame();
  const uint64_t slot1 = STATS_STRIDE;
  PutWord(rec, slot1 + STAT_POINTS + STAT_WORD_TYPE, STAT_TYPE_AVERAGE);
  PutWord(rec, slot1 + STAT_POINTS + STAT_WORD_VALUE, 10);
  PutWord(rec, slot1 + STAT_POINTS + STAT_WORD_COUNT, 4);
  EXPECT_EQ_U32(ReadStatValue(rec.data(), 1, STAT_POINTS), 2u);
  // A zero count reads 0, never a division by zero.
  PutWord(rec, slot1 + STAT_POINTS + STAT_WORD_COUNT, 0);
  EXPECT_EQ_U32(ReadStatValue(rec.data(), 1, STAT_POINTS), 0u);
}

void TestPossessionIsAFloatInTheValueWord() {
  using namespace nevr_game_offsets;
  std::vector<char> rec = NetGame();
  const uint64_t slot1 = STATS_STRIDE;
  PutFloat(rec, slot1 + STAT_POSSESSION_TIME + STAT_WORD_VALUE, 12.5f);
  EXPECT_EQ_U32(ReadStatFloat(rec.data(), 1, STAT_POSSESSION_TIME), 12u);
}

}  // namespace

int main() {
  TestReadsEachStatAtTheWritersOffset();
  TestAverageEntryDividesValueByCount();
  TestPossessionIsAFloatInTheValueWord();
  if (g_failures != 0) {
    std::printf("test_telemetry_stats: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_telemetry_stats: all checks passed\n");
  return 0;
}

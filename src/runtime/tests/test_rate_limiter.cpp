#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "runtime/log/rate_limiter.h"

using nevr_rate_limiter::RateLimiter;
using nevr_rate_limiter::RateVerdict;

namespace {

constexpr const char* kMsg = "[EVR] peer timed out";

/* Feeds `n` copies of `msg` at time `now`; returns how many were collapsed. */
int Burst(RateLimiter& rl, const char* msg, int n, uint64_t now) {
  char summary[256];
  int collapsed = 0;
  for (int i = 0; i < n; i++) {
    if (rl.Check(msg, static_cast<int>(std::strlen(msg)), now, summary, sizeof(summary)) == RateVerdict::Collapse) {
      collapsed++;
    }
  }
  return collapsed;
}

}  // namespace

TEST(RateLimiter, FirstThreeInAWindowPassAndTheRestCollapse) {
  RateLimiter rl;
  EXPECT_EQ(Burst(rl, kMsg, 20, 100), 17);
}

TEST(RateLimiter, DigitsDoNotChangeALinesIdentity) {
  RateLimiter rl;
  char summary[256];
  int collapsed = 0;
  for (int i = 0; i < 10; i++) {
    const std::string msg = "ExitProcess(" + std::to_string(i) + ") call #" + std::to_string(40 + i);
    if (rl.Check(msg.c_str(), static_cast<int>(msg.size()), 100, summary, sizeof(summary)) == RateVerdict::Collapse) {
      collapsed++;
    }
  }
  EXPECT_EQ(collapsed, 7);
}

TEST(RateLimiter, AFollowingLineAfterTheWindowReportsTheCollapsedCount) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  EXPECT_EQ(rl.Check(kMsg, static_cast<int>(std::strlen(kMsg)), 106, summary, sizeof(summary)), RateVerdict::Emit);
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=17 window_s=6 msg=\"[EVR] peer timed out\"");
}

TEST(RateLimiter, ABurstThatGoesQuietIsReportedOnceItsWindowHasExpired) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  EXPECT_FALSE(rl.TakePending(103, false, summary, sizeof(summary))) << "the window is still open";
  EXPECT_STREQ(summary, "");
  ASSERT_TRUE(rl.TakePending(106, false, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=17 window_s=6 msg=\"[EVR] peer timed out\"");
  EXPECT_FALSE(rl.TakePending(106, false, summary, sizeof(summary))) << "a count is reported once";
}

TEST(RateLimiter, AReportedBurstStartsAFreshWindowOnItsNextLine) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  ASSERT_TRUE(rl.TakePending(106, false, summary, sizeof(summary)));
  EXPECT_EQ(Burst(rl, kMsg, 4, 107), 1) << "three pass again, one collapses";
  ASSERT_TRUE(rl.TakePending(108, true, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=1 window_s=1 msg=\"[EVR] peer timed out\"");
}

TEST(RateLimiter, ShutdownReportsAWindowThatIsStillOpen) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  ASSERT_TRUE(rl.TakePending(101, true, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=17 window_s=1 msg=\"[EVR] peer timed out\"");
  EXPECT_FALSE(rl.TakePending(101, true, summary, sizeof(summary)));
}

TEST(RateLimiter, NothingIsReportedWhenNothingWasCollapsed) {
  RateLimiter rl;
  Burst(rl, kMsg, 3, 100);
  char summary[256];
  EXPECT_FALSE(rl.TakePending(200, true, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "");
}

TEST(RateLimiter, EveryQuietBurstIsReportedByItsOwnLine) {
  RateLimiter rl;
  Burst(rl, "[EVR] socket reset", 10, 100);
  Burst(rl, "[EVR] voip underrun", 6, 100);
  char summary[256];
  std::string all;
  int reports = 0;
  while (rl.TakePending(110, false, summary, sizeof(summary))) {
    all += summary;
    all += '\n';
    reports++;
  }
  EXPECT_EQ(reports, 2);
  EXPECT_NE(all.find("count=7 window_s=10 msg=\"[EVR] socket reset\""), std::string::npos) << all;
  EXPECT_NE(all.find("count=3 window_s=10 msg=\"[EVR] voip underrun\""), std::string::npos) << all;
}

TEST(RateLimiter, AClockThatMovedBackDoesNotMakeAWindowLookExpired) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  EXPECT_FALSE(rl.TakePending(90, false, summary, sizeof(summary)));
  ASSERT_TRUE(rl.TakePending(90, true, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=17 window_s=0 msg=\"[EVR] peer timed out\"");
}

namespace {

/* The line the game's scripts write: "realdiv(%x) divide by zero" (searched all 1,041 binaries in project echovr: the string is in
 * the script DLLs), after the filter put "[EVR] " in front of it. `value` is the %x argument. */
std::string Realdiv(unsigned value) {
  char text[96];
  std::snprintf(text, sizeof(text), "[EVR] realdiv(%x) divide by zero", value);
  return text;
}

/* Returns how many of the `n` lines were let through; every summary a Check produced is appended to `summaries`. */
int FeedRealdiv(RateLimiter& rl, int n, uint64_t now, unsigned first_value, std::string& summaries) {
  int emitted = 0;
  char summary[256];
  for (int i = 0; i < n; i++) {
    const std::string line = Realdiv(first_value + 0x9e3779b1u * static_cast<unsigned>(i));
    if (rl.Check(line.c_str(), static_cast<int>(line.size()), now, summary, sizeof(summary)) == RateVerdict::Emit) emitted++;
    summaries += summary;
  }
  return emitted;
}

}  // namespace

/* E1 (STARBURST): 1000 of its divide-by-zero lines, the value changing every line and holding letters as well as digits, are
 * three lines and one counted line, not a thousand and not none. */
TEST(RateLimiterRealdiv, ThousandLinesWithVaryingHexAreThreeLinesAndOneCount) {
  RateLimiter rl;
  char summary[256];
  std::string during;
  EXPECT_EQ(FeedRealdiv(rl, 1000, 100, 0x1a2b3cu, during), 3);
  EXPECT_EQ(during, "") << "nothing is reported while the window is open";
  ASSERT_TRUE(rl.TakePending(106, false, summary, sizeof(summary)));
  EXPECT_STREQ(summary,
               "[NEVR.LOGFILTER] repeated count=997 window_s=6 msg=\"[EVR] realdiv(1a2b3c) divide by zero\"");
  EXPECT_FALSE(rl.TakePending(106, false, summary, sizeof(summary))) << "one counted line, not one per value";
}

TEST(RateLimiterRealdiv, EachWindowOfAContinuingBurstIsThreeLinesAndOneCount) {
  RateLimiter rl;
  std::string first_window;
  std::string second_window;
  EXPECT_EQ(FeedRealdiv(rl, 600, 100, 0xabcdefu, first_window), 3);
  EXPECT_EQ(first_window, "");
  EXPECT_EQ(FeedRealdiv(rl, 600, 105, 0xfedcbau, second_window), 3);
  EXPECT_EQ(second_window, "[NEVR.LOGFILTER] repeated count=597 window_s=5 msg=\"[EVR] realdiv(abcdef) divide by zero\"");
}

TEST(RateLimiterRealdiv, ARealdivBurstDoesNotHideAnUnrelatedLine) {
  RateLimiter rl;
  char summary[256];
  std::string ignored;
  FeedRealdiv(rl, 1000, 100, 0x77u, ignored);
  EXPECT_EQ(rl.Check(kMsg, static_cast<int>(std::strlen(kMsg)), 100, summary, sizeof(summary)), RateVerdict::Emit);
}

/* The format is the key, not "any hex": other lines that differ in hex digits stay separate lines. */
TEST(RateLimiterRealdiv, OtherLinesThatDifferInHexAreNotMerged) {
  RateLimiter rl;
  char summary[256];
  int collapsed = 0;
  const char* values[] = {"ab", "cd", "ef", "a1", "b2", "c3", "d4", "e5", "f6", "9a"};
  for (const char* v : values) {
    const std::string line = std::string("[EVR] shield(") + v + ") divide by zero";
    if (rl.Check(line.c_str(), static_cast<int>(line.size()), 100, summary, sizeof(summary)) == RateVerdict::Collapse) collapsed++;
  }
  EXPECT_EQ(collapsed, 0);
}

TEST(RateLimiterRealdiv, ALineWhoseValueIsNotHexIsNotTheFormat) {
  RateLimiter rl;
  char summary[256];
  int collapsed = 0;
  const char* values[] = {"zz", "yy", "xx", "ww", "vv", "uu", "tt", "ss"};
  for (const char* v : values) {
    const std::string line = std::string("[EVR] realdiv(") + v + ") divide by zero";
    if (rl.Check(line.c_str(), static_cast<int>(line.size()), 100, summary, sizeof(summary)) == RateVerdict::Collapse) collapsed++;
  }
  EXPECT_EQ(collapsed, 0);
}

TEST(CollapseFormatRules, TheFormatMatchesWhereverItSitsInTheLine) {
  using nevr_collapse_format::Match;
  const std::string bare = "realdiv(0) divide by zero";
  const std::string annotated = "[EVR] realdiv(DEADbeef) divide by zero";
  const std::string trailing = "[EVR] realdiv(1f) divide by zero (script a9db89899292a98f)";
  const std::string sixteen = "realdiv(0123456789abcdef) divide by zero";
  for (const std::string* line : {&bare, &annotated, &trailing, &sixteen}) {
    const char* name = Match(line->c_str(), static_cast<int>(line->size()));
    ASSERT_NE(name, nullptr) << *line;
    EXPECT_STREQ(name, "script_realdiv") << *line;
  }
}

TEST(CollapseFormatRules, ALineThatIsNotTheFormatHasNoRule) {
  using nevr_collapse_format::Match;
  const char* lines[] = {
      "realdiv() divide by zero",                    /* no value */
      "realdiv(zz) divide by zero",                  /* not hex */
      "realdiv(0123456789abcdef0) divide by zero",   /* 17 digits */
      "realdiv(1a) divided",                         /* another tail */
      "realdiv(1a divide by zero",                   /* no closing parenthesis */
      "[EVR] peer timed out",
      "",
  };
  for (const char* line : lines) {
    EXPECT_EQ(Match(line, static_cast<int>(std::strlen(line))), nullptr) << line;
  }
  EXPECT_EQ(Match(nullptr, 5), nullptr);
  const std::string cut = "realdiv(1a) divide by zero";
  EXPECT_EQ(Match(cut.c_str(), static_cast<int>(cut.size()) - 1), nullptr) << "the tail must be inside len";
}

/* #2214: a line the default suppress list deletes is still counted, under the entry that matched it. The filter hands the limiter the
 * entry's text and never lets the line through, so a flood under one entry reads as that entry in the summary. */
namespace {

int CountFlood(RateLimiter& rl, const char* entry, int n, uint64_t now, std::string& summaries) {
  int passed = 0;
  char summary[256];
  for (int i = 0; i < n; i++) {
    const std::string line = std::string("[EVR] ") + entry + " " + std::to_string(i);
    if (rl.CountSuppressed(entry, line.c_str(), static_cast<int>(line.size()), now, summary, sizeof(summary)) ==
        RateVerdict::Emit) {
      passed++;
    }
    summaries += summary;
  }
  return passed;
}

}  // namespace

TEST(RateLimiterSuppressed, AFloodUnderOneEntryIsReportedByThatEntryAndNothingPasses) {
  RateLimiter rl;
  char summary[256];
  std::string during;
  EXPECT_EQ(CountFlood(rl, "Is Bot", 500, 100, during), 0) << "a suppressed line is never let through";
  EXPECT_EQ(during, "");
  ASSERT_TRUE(rl.TakePending(106, false, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] suppressed count=500 window_s=6 entry=\"Is Bot\"");
  EXPECT_FALSE(rl.TakePending(106, false, summary, sizeof(summary)));
}

TEST(RateLimiterSuppressed, EachEntryHasItsOwnCount) {
  RateLimiter rl;
  std::string ignored;
  CountFlood(rl, "Is Bot", 40, 100, ignored);
  CountFlood(rl, "Is Ghost", 7, 100, ignored);
  char summary[256];
  std::string all;
  while (rl.TakePending(110, false, summary, sizeof(summary))) all += std::string(summary) + "\n";
  EXPECT_NE(all.find("suppressed count=40 window_s=10 entry=\"Is Bot\""), std::string::npos) << all;
  EXPECT_NE(all.find("suppressed count=7 window_s=10 entry=\"Is Ghost\""), std::string::npos) << all;
}

TEST(RateLimiterSuppressed, ASuppressedEntryDoesNotUseUpAPassedLinesBudget) {
  RateLimiter rl;
  std::string ignored;
  CountFlood(rl, "Is Bot", 100, 100, ignored);
  char summary[256];
  const std::string same = "[EVR] Is Bot 1";
  EXPECT_EQ(rl.Check(same.c_str(), static_cast<int>(same.size()), 100, summary, sizeof(summary)), RateVerdict::Emit);
}

TEST(RateLimiterSuppressed, AWindowThatClosesOnALaterCountReportsTheOldOne) {
  RateLimiter rl;
  char summary[256];
  std::string ignored;
  CountFlood(rl, "Is Bot", 9, 100, ignored);
  const std::string line = "[EVR] Is Bot again";
  rl.CountSuppressed("Is Bot", line.c_str(), static_cast<int>(line.size()), 106, summary, sizeof(summary));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] suppressed count=9 window_s=6 entry=\"Is Bot\"");
}

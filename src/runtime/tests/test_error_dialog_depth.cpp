// #2245: the game's error dialog marks its thread while it runs (log/error_dialog_depth.h), and the filter's level
// gate never drops a level it has no name for (core/log_level.h). The hook (log/error_dialog_level.cpp) runs the
// original dialog through nevr_error_dialog::Run, and ShouldSuppress (log/builtin_filter.cpp) asks
// nevr_log_level::DroppedByMinLevel; these tests drive those two functions.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

#include "core/log_level.h"
#include "runtime/log/error_dialog_depth.h"

TEST(ErrorDialogDepth, TheThreadIsInsideOnlyWhileTheDialogRuns) {
  ASSERT_FALSE(nevr_error_dialog::Inside());
  bool insideDuring = false;
  nevr_error_dialog::Run([&] { insideDuring = nevr_error_dialog::Inside(); });
  EXPECT_TRUE(insideDuring);
  EXPECT_FALSE(nevr_error_dialog::Inside());
}

// An exception that leaves the dialog leaves the thread outside it; otherwise every later line on the thread would
// be recorded at error.
TEST(ErrorDialogDepth, AnExceptionThroughTheDialogLeavesTheThreadOutside) {
  ASSERT_FALSE(nevr_error_dialog::Inside());
  EXPECT_THROW(nevr_error_dialog::Run([] { throw std::runtime_error("dialog unwound"); }), std::runtime_error);
  EXPECT_FALSE(nevr_error_dialog::Inside());
}

// The runtime's one longjmp (crash_recovery.cpp, N125) leaves from a vectored exception handler, which runs no
// destructor; its landing site resets the mark (tools/tests/test_error_dialog_wiring.py checks the call is there).
TEST(ErrorDialogDepth, TheLongjmpLandingResetsAMarkLeftBehind) {
  nevr_error_dialog::t_depth = 2;  // what a longjmp out of a nested Run leaves
  ASSERT_TRUE(nevr_error_dialog::Inside());
  nevr_error_dialog::ResetAfterLongjmp();
  EXPECT_FALSE(nevr_error_dialog::Inside());
}

TEST(MinLevelGate, ANamedLevelBelowTheMinimumIsDropped) {
  EXPECT_TRUE(nevr_log_level::DroppedByMinLevel(nevr_log_level::kDebug, nevr_log_level::kInfo));
  EXPECT_TRUE(nevr_log_level::DroppedByMinLevel(nevr_log_level::kWarning, nevr_log_level::kError));
}

TEST(MinLevelGate, ANamedLevelAtOrAboveTheMinimumIsKept) {
  for (const std::uint32_t level : {nevr_log_level::kInfo, nevr_log_level::kWarning, nevr_log_level::kError}) {
    EXPECT_FALSE(nevr_log_level::DroppedByMinLevel(level, nevr_log_level::kInfo)) << "level " << level;
  }
  EXPECT_FALSE(nevr_log_level::DroppedByMinLevel(nevr_log_level::kError, nevr_log_level::kError));
}

// A level the mapping has no name for is written "unknown" with its raw value; min_level never hides it.
TEST(MinLevelGate, ALevelWithNoNameIsNeverDropped) {
  for (const std::uint32_t raw : {0u, 3u, 5u, 1000u}) {
    EXPECT_FALSE(nevr_log_level::DroppedByMinLevel(raw, nevr_log_level::kError)) << "raw " << raw;
  }
}

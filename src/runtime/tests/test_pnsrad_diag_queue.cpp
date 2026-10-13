#include <gtest/gtest.h>

#include "runtime/patch/pnsrad_diag_queue.h"

TEST(PnsradDiagQueue, InstallsOnlyWhenDrainedAndOnlyOnce) {
  PnsradDiagQueue queue;
  int installs = 0;
  queue.NoteLoaded(0x180000000ULL);
  EXPECT_EQ(installs, 0);
  EXPECT_TRUE(queue.Drain([&](uintptr_t base) {
    EXPECT_EQ(base, 0x180000000ULL);
    ++installs;
  }));
  EXPECT_EQ(installs, 1);
  EXPECT_FALSE(queue.Drain([&](uintptr_t) { ++installs; }));
  EXPECT_EQ(installs, 1);
}

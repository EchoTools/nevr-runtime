// #341: a client whose X window is destroyed from the outside quits; nothing else does.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "runtime/frame/window_watch.h"

namespace {

using nevr_window_watch::kGraceMs;
using nevr_window_watch::Sample;
using nevr_window_watch::Verdict;
using nevr_window_watch::Watch;

constexpr Sample kAlive = {true, true};
constexpr Sample kXGone = {true, false};
constexpr Sample kHwndGone = {false, false};

TEST(WindowWatch, ArmsOnTheFirstSightOfTheXWindowOnly) {
  Watch watch;
  EXPECT_EQ(watch.Observe(kAlive, 1000), Verdict::Armed);
  EXPECT_TRUE(watch.Armed());
  EXPECT_EQ(watch.Observe(kAlive, 2000), Verdict::Nothing);
}

TEST(WindowWatch, AWindowWithoutAnXWindowNeverArmsSoNativeWindowsIsUntouched) {
  Watch watch;
  for (uint64_t t = 0; t < 600000; t += 1000) EXPECT_EQ(watch.Observe(kXGone, t), Verdict::Nothing);
  EXPECT_FALSE(watch.Armed());
  EXPECT_FALSE(watch.Fired());
}

TEST(WindowWatch, RequestsQuitWhenTheXWindowStaysGoneForTheGrace) {
  Watch watch;
  EXPECT_EQ(watch.Observe(kAlive, 0), Verdict::Armed);
  EXPECT_EQ(watch.Observe(kXGone, 5000), Verdict::Nothing);                  // gone since 5000
  EXPECT_EQ(watch.Observe(kXGone, 5000 + kGraceMs - 1), Verdict::Nothing);   // not long enough
  EXPECT_EQ(watch.Observe(kXGone, 5000 + kGraceMs), Verdict::RequestQuit);
  EXPECT_TRUE(watch.Fired());
}

TEST(WindowWatch, AFlickerShorterThanTheGraceDoesNotQuit) {
  Watch watch;
  EXPECT_EQ(watch.Observe(kAlive, 0), Verdict::Armed);
  EXPECT_EQ(watch.Observe(kXGone, 1000), Verdict::Nothing);
  EXPECT_EQ(watch.Observe(kAlive, 2000), Verdict::Nothing);   // back: the clock restarts
  EXPECT_EQ(watch.Observe(kXGone, 3000), Verdict::Nothing);
  EXPECT_EQ(watch.Observe(kXGone, 3000 + kGraceMs - 1), Verdict::Nothing);
  EXPECT_FALSE(watch.Fired());
}

TEST(WindowWatch, QuitsOnceOnly) {
  Watch watch;
  watch.Observe(kAlive, 0);
  watch.Observe(kXGone, 1000);
  EXPECT_EQ(watch.Observe(kXGone, 1000 + kGraceMs), Verdict::RequestQuit);
  EXPECT_EQ(watch.Observe(kXGone, 10000), Verdict::Nothing);
}

TEST(WindowWatch, AnInvalidHwndIsTheGamesOwnQuitPathAndAddsNothing) {
  Watch watch;
  watch.Observe(kAlive, 0);
  EXPECT_EQ(watch.Observe(kHwndGone, 1000), Verdict::Nothing);
  EXPECT_EQ(watch.Observe(kHwndGone, 1000 + 10 * kGraceMs), Verdict::Nothing);
  EXPECT_FALSE(watch.Fired());
}

TEST(WindowWatch, QuitSiteBytesAreCheckedBeforeAnyWrite) {
  std::vector<uint8_t> site(std::begin(nevr_window_watch::kQuitSiteBytes), std::end(nevr_window_watch::kQuitSiteBytes));
  EXPECT_TRUE(nevr_window_watch::QuitSiteMatches(site.data()));
  site[15] ^= 0x01;  // the quit flag's offset changed in another build
  EXPECT_FALSE(nevr_window_watch::QuitSiteMatches(site.data()));
  EXPECT_FALSE(nevr_window_watch::QuitSiteMatches(nullptr));
}

TEST(WindowWatch, ConstantsMatchTheQuitSiteBytes) {
  // The site's rip-relative displacement (bytes 3..6, little endian) reaches the engine global from the end of
  // the 7-byte MOV; the flag offset is bytes 14..17 of the MOV DWORD [RAX+disp32],1.
  const uint32_t displacement = static_cast<uint32_t>(nevr_window_watch::kQuitSiteBytes[3]) |
                                (static_cast<uint32_t>(nevr_window_watch::kQuitSiteBytes[4]) << 8) |
                                (static_cast<uint32_t>(nevr_window_watch::kQuitSiteBytes[5]) << 16) |
                                (static_cast<uint32_t>(nevr_window_watch::kQuitSiteBytes[6]) << 24);
  EXPECT_EQ(nevr_window_watch::kQuitSiteRva + 7 + displacement, nevr_window_watch::kEngineGlobalRva);
  const uint32_t flagOffset = static_cast<uint32_t>(nevr_window_watch::kQuitSiteBytes[14]) |
                              (static_cast<uint32_t>(nevr_window_watch::kQuitSiteBytes[15]) << 8) |
                              (static_cast<uint32_t>(nevr_window_watch::kQuitSiteBytes[16]) << 16) |
                              (static_cast<uint32_t>(nevr_window_watch::kQuitSiteBytes[17]) << 24);
  EXPECT_EQ(flagOffset, nevr_window_watch::kQuitFlagOffset);
}

}  // namespace

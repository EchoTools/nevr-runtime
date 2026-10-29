// When a button press skips the splash screen (patch/splash_skip_rules.h). The hook that applies the decisions is
// patch/splash_skip.cpp; its byte checks, the widget reads and the dispatch need the game and are proven by a live run.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/patch/splash_skip_rules.h"

namespace {

using nevr_splash_skip::FindContinueElement;
using nevr_splash_skip::kContinueClickedSymbol;
using nevr_splash_skip::kLockoutMs;
using nevr_splash_skip::kMinPresentMs;
using nevr_splash_skip::PressedButton;
using nevr_splash_skip::Skipper;

constexpr std::uint64_t kStart = 100000;

// The symbol is the game's CSymbol64 hash of the event name the splash page script listens for.
TEST(SplashSkip, TheContinueSymbolIsTheHashOfContinueClicked) {
  EXPECT_EQ(kContinueClickedSymbol, EchoVR::CSymbol64Hash("continue_clicked"));
}

TEST(SplashSkip, FindsTheContinueElementAmongTheWidgetsElements) {
  const std::vector<std::uint64_t> symbols = {~0ULL, EchoVR::CSymbol64Hash("learn_clicked"), kContinueClickedSymbol, 7};
  EXPECT_EQ(FindContinueElement(symbols.size(), [&](std::size_t i) { return symbols[i]; }), 2);
}

TEST(SplashSkip, AWidgetWithoutTheContinueElementHasNone) {
  const std::vector<std::uint64_t> symbols = {~0ULL, EchoVR::CSymbol64Hash("learn_clicked")};
  EXPECT_EQ(FindContinueElement(symbols.size(), [&](std::size_t i) { return symbols[i]; }), -1);
  EXPECT_EQ(FindContinueElement(0, [](std::size_t) { return kContinueClickedSymbol; }), -1);
}

TEST(SplashSkip, ThePressedButtonIsTheFirstOfTheClickIdsThatIsDown) {
  EXPECT_EQ(PressedButton([](std::uint64_t id) { return id == 4; }), 4);
  EXPECT_EQ(PressedButton([](std::uint64_t id) { return id == 5 || id == 1; }), 1);
  EXPECT_EQ(PressedButton([](std::uint64_t id) { return id == 2 || id == 3 || id == 6; }), -1);
  EXPECT_EQ(PressedButton([](std::uint64_t) { return false; }), -1);
}

TEST(SplashSkip, APressWhileTheButtonIsLiveSkips) {
  Skipper s;
  EXPECT_FALSE(s.Step(true, false, kStart));
  EXPECT_TRUE(s.Step(true, true, kStart + kMinPresentMs));
}

// The press that dismissed the page before this one must not skip this one.
TEST(SplashSkip, APressBeforeTheButtonHasBeenLiveLongEnoughDoesNotSkip) {
  Skipper s;
  EXPECT_FALSE(s.Step(true, false, kStart));
  EXPECT_FALSE(s.Step(true, true, kStart + kMinPresentMs - 1));
}

TEST(SplashSkip, AHeldButtonDoesNotSkipUntilItIsPressedAgain) {
  Skipper s;
  EXPECT_FALSE(s.Step(false, true, kStart));
  EXPECT_FALSE(s.Step(true, true, kStart + 10));
  EXPECT_FALSE(s.Step(true, true, kStart + kMinPresentMs + 100));
  EXPECT_FALSE(s.Step(true, false, kStart + kMinPresentMs + 200));
  EXPECT_TRUE(s.Step(true, true, kStart + kMinPresentMs + 300));
}

TEST(SplashSkip, NothingSkipsWhileTheButtonIsNotLive) {
  Skipper s;
  EXPECT_FALSE(s.Step(false, false, kStart));
  EXPECT_FALSE(s.Step(false, true, kStart + kMinPresentMs + 1));
  EXPECT_FALSE(s.Step(false, false, kStart + kMinPresentMs + 2));
  EXPECT_FALSE(s.Step(false, true, kStart + 2 * kMinPresentMs));
}

// The widget recomputes its button flags only every few updates, so one press shows as an edge on several calls.
TEST(SplashSkip, AStaleEdgeAfterASkipDoesNotSkipAgain) {
  Skipper s;
  s.Step(true, false, kStart);
  ASSERT_TRUE(s.Step(true, true, kStart + kMinPresentMs));
  EXPECT_FALSE(s.Step(true, true, kStart + kMinPresentMs + 16));
  EXPECT_FALSE(s.Step(true, false, kStart + kMinPresentMs + 32));
  EXPECT_FALSE(s.Step(true, true, kStart + kMinPresentMs + kLockoutMs - 1));
  EXPECT_FALSE(s.Step(true, false, kStart + kMinPresentMs + kLockoutMs));
  EXPECT_TRUE(s.Step(true, true, kStart + kMinPresentMs + kLockoutMs + 100));
}

TEST(SplashSkip, ALeavingAndReturningButtonMustBeLiveAgainBeforeASkip) {
  Skipper s;
  s.Step(true, false, kStart);
  s.Step(false, false, kStart + 5000);
  s.Step(true, false, kStart + 6000);
  EXPECT_FALSE(s.Step(true, true, kStart + 6000 + kMinPresentMs - 1));
  s.Step(true, false, kStart + 6000 + kMinPresentMs);
  EXPECT_TRUE(s.Step(true, true, kStart + 6000 + kMinPresentMs + 1));
}

TEST(SplashSkip, TheLogLineReportsHowLongTheButtonWasLive) {
  Skipper s;
  EXPECT_EQ(s.PresentForMs(kStart), 0u);
  s.Step(true, false, kStart);
  EXPECT_EQ(s.PresentForMs(kStart + 900), 900u);
  s.Step(false, false, kStart + 1000);
  EXPECT_EQ(s.PresentForMs(kStart + 1100), 0u);
}

}  // namespace

// #515: a dedicated server's time-step arguments are removed from the command line the game parses.
// lifecycle/cmdline_strip.h is the pure function GameMainWrapperHook (crash_recovery.cpp) calls on the
// string echovr.exe's Main_ (0x1400cd550) turns into its CMainArgs.
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/globals.h"
#include "runtime/lifecycle/cmdline_strip.h"
#include "runtime/lifecycle/server_timestep.h"

namespace {

using nevr_cmdline::StripServerTimestepArgs;
using nevr_cmdline::Tokenize;

// The tokens of `commandLine`, as the game's parser sees them.
std::vector<std::string> TextsOf(const std::string& commandLine) {
  std::vector<std::string> texts;
  for (const nevr_cmdline::Token& t : Tokenize(commandLine)) texts.emplace_back(t.text);
  return texts;
}

bool Has(const std::vector<std::string>& texts, const std::string& token) {
  for (const std::string& t : texts) {
    if (t == token) return true;
  }
  return false;
}

}  // namespace

TEST(CmdlineStrip, AServerLosesFixedtimestep) {
  const auto r = StripServerTimestepArgs("echovr.exe -server -fixedtimestep -noconsole");
  EXPECT_EQ(r.commandLine, "echovr.exe -server -noconsole");
  EXPECT_FALSE(Has(TextsOf(r.commandLine), "-fixedtimestep"));
  EXPECT_EQ(r.removed.size(), 1u);
}

TEST(CmdlineStrip, AServerLosesTimestepAndItsValue) {
  const auto r = StripServerTimestepArgs("echovr.exe -server -timestep 120 -noconsole");
  EXPECT_EQ(r.commandLine, "echovr.exe -server -noconsole");
  const auto texts = TextsOf(r.commandLine);
  EXPECT_FALSE(Has(texts, "-timestep"));
  EXPECT_FALSE(Has(texts, "120")) << "the value goes with its option";
  EXPECT_EQ(r.removed.size(), 2u);
}

TEST(CmdlineStrip, BothTogetherInAnyOrderAndAtTheEnd) {
  const auto r = StripServerTimestepArgs("echovr.exe -fixedtimestep -server -timestep 60");
  EXPECT_EQ(r.commandLine, "echovr.exe -server");
  EXPECT_EQ(r.removed.size(), 3u);
}

TEST(CmdlineStrip, ATimestepWithoutAValueTakesNoNeighbour) {
  // -noconsole is an option, not -timestep's value.
  const auto r = StripServerTimestepArgs("echovr.exe -server -timestep -noconsole");
  EXPECT_EQ(r.commandLine, "echovr.exe -server -noconsole");
  EXPECT_EQ(r.removed.size(), 1u);
  // A negative number is a value, as the game's parser reads it.
  const auto n = StripServerTimestepArgs("echovr.exe -server -timestep -5");
  EXPECT_EQ(n.commandLine, "echovr.exe -server");
}

TEST(CmdlineStrip, TheGamesNameLookupIsMatched) {
  // The game skips leading dashes and lower-cases (CSymbol64::Lookup), so these reach the same option.
  const auto r = StripServerTimestepArgs("echovr.exe -server -FixedTimeStep --fixedtimestep +TimeStep 120");
  EXPECT_EQ(r.commandLine, "echovr.exe -server");
  EXPECT_EQ(r.removed.size(), 4u);
}

TEST(CmdlineStrip, OtherArgumentsAndTheirValuesSurviveUnchanged) {
  const auto r = StripServerTimestepArgs(
      "C:\\g\\echovr.exe -server -config \"C:\\my dir\\c.json\" -timestep 120 -region us-east -fixedtimestep");
  EXPECT_EQ(r.commandLine, "C:\\g\\echovr.exe -server -config \"C:\\my dir\\c.json\" -region us-east");
}

TEST(CmdlineStrip, ANameThatOnlyContainsTimestepIsLeftAlone) {
  const auto r = StripServerTimestepArgs("echovr.exe -server -mytimestep 5 -timestepper");
  EXPECT_EQ(r.commandLine, "echovr.exe -server -mytimestep 5 -timestepper");
  EXPECT_TRUE(r.removed.empty());
}

TEST(CmdlineStrip, AClientIsUntouched) {
  // No -server token: the text comes back byte for byte, whatever the arguments are.
  const std::string client = "echovr.exe -windowed -fixedtimestep -timestep 120  -noovr";
  const auto r = StripServerTimestepArgs(client);
  EXPECT_EQ(r.commandLine, client);
  EXPECT_TRUE(r.removed.empty());
  EXPECT_TRUE(Has(TextsOf(r.commandLine), "-fixedtimestep"));
  EXPECT_TRUE(Has(TextsOf(r.commandLine), "-timestep"));
  EXPECT_TRUE(Has(TextsOf(r.commandLine), "120"));
}

TEST(CmdlineStrip, ServerIsTheExactToken) {
  // "-servers" and a value named -server are decided the way boot.cpp decides g_isServer: by the token.
  EXPECT_TRUE(nevr_cmdline::HasServerToken("echovr.exe -server"));
  EXPECT_FALSE(nevr_cmdline::HasServerToken("echovr.exe -servers -fixedtimestep"));
  EXPECT_EQ(StripServerTimestepArgs("echovr.exe -servers -fixedtimestep").removed.size(), 0u);
}

TEST(CmdlineStrip, ACommandLineWithNothingToRemoveIsReturnedAsIs) {
  const std::string text = "echovr.exe   -server   -noconsole ";
  const auto r = StripServerTimestepArgs(text);
  EXPECT_EQ(r.commandLine, text);
  EXPECT_TRUE(r.removed.empty());
}

TEST(CmdlineStrip, TheServerStepAfterTheStripIsTheConfiguredRate) {
  // The step NEVR sets once the arguments are gone: 1,000,000 / g_headlessTickRateHz. The rate defaults to
  // 120 Hz (core/globals.cpp), which is 8333 us.
  EXPECT_EQ(g_headlessTickRateHz, 120u);
  EXPECT_EQ(nevr_server_timestep::StepMicroseconds(g_headlessTickRateHz), 8333u);
}

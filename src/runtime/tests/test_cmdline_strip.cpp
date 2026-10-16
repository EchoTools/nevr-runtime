// #515: a dedicated server's time-step arguments are removed from the command line the game parses.
// lifecycle/cmdline_strip.h is the pure code GameMainWrapperHook (crash_recovery.cpp) calls on the
// string echovr.exe's Main_ (0x1400cd550) turns into its CMainArgs.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/globals.h"
#include "runtime/lifecycle/cmdline_strip.h"
#include "runtime/lifecycle/server_timestep.h"

namespace {

using nevr_cmdline::HasServerToken;
using nevr_cmdline::HasTimestepOption;
using nevr_cmdline::PrepareForGame;
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
  EXPECT_EQ(r.options, std::vector<std::string>{"-fixedtimestep"});
}

TEST(CmdlineStrip, AServerLosesTimestepAndItsValue) {
  const auto r = StripServerTimestepArgs("echovr.exe -server -timestep 120 -noconsole");
  EXPECT_EQ(r.commandLine, "echovr.exe -server -noconsole");
  const auto texts = TextsOf(r.commandLine);
  EXPECT_FALSE(Has(texts, "-timestep"));
  EXPECT_FALSE(Has(texts, "120")) << "the value goes with its option";
  EXPECT_EQ(r.removed.size(), 2u);
  EXPECT_EQ(r.options, std::vector<std::string>{"-timestep"});
}

TEST(CmdlineStrip, BothTogetherInAnyOrderAndAtTheEnd) {
  const auto r = StripServerTimestepArgs("echovr.exe -fixedtimestep -server -timestep 60");
  EXPECT_EQ(r.commandLine, "echovr.exe -server");
  EXPECT_EQ(r.removed.size(), 3u);
  EXPECT_EQ(r.options, (std::vector<std::string>{"-fixedtimestep", "-timestep"}));
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
  EXPECT_EQ(r.options, (std::vector<std::string>{"-fixedtimestep", "-fixedtimestep", "-timestep"}))
      << "the canonical names are reported, whatever the spelling";
}

TEST(CmdlineStrip, TheGames0x96DashIsADash) {
  // 0x96 starts an option and is skipped by a name lookup, exactly like '-' (FUN_1400d9000, 0x14011672F...).
  const auto r = StripServerTimestepArgs(std::string("echovr.exe -server \x96") + "timestep 5 -noconsole");
  EXPECT_EQ(r.commandLine, "echovr.exe -server -noconsole");
  const auto f = StripServerTimestepArgs(std::string("echovr.exe -server -x \x96\x96") + "fixedtimestep");
  EXPECT_EQ(f.commandLine, "echovr.exe -server -x");
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
  // No -server option: the text comes back byte for byte, whatever the arguments are.
  const std::string client = "echovr.exe -windowed -fixedtimestep -timestep 120  -noovr";
  const auto r = StripServerTimestepArgs(client);
  EXPECT_EQ(r.commandLine, client);
  EXPECT_TRUE(r.removed.empty());
  EXPECT_TRUE(r.options.empty());
  EXPECT_TRUE(Has(TextsOf(r.commandLine), "-fixedtimestep"));
  EXPECT_TRUE(Has(TextsOf(r.commandLine), "-timestep"));
  EXPECT_TRUE(Has(TextsOf(r.commandLine), "120"));
}

TEST(CmdlineStrip, ServerIsTheExactOption) {
  EXPECT_TRUE(HasServerToken("echovr.exe -server"));
  EXPECT_FALSE(HasServerToken("echovr.exe -servers -fixedtimestep"));
  EXPECT_EQ(StripServerTimestepArgs("echovr.exe -servers -fixedtimestep").removed.size(), 0u);
}

TEST(CmdlineStrip, TheServerRuleIsTheGamesParserNotBootCpps) {
  // N3: StripServerTimestepArgs reads `-server` the way the game's tokenizer does (an option token), which is
  // the string it edits. boot.cpp's CommandLineToArgvW reading differs on quotes; the strip's reading wins
  // for the strip, and boot.cpp's log follows what the hook recorded.
  // A bracketed `[-server]` is a parameter (not an option) to the game: not a server.
  EXPECT_FALSE(HasServerToken("echovr.exe [-server] -fixedtimestep"));
  EXPECT_EQ(StripServerTimestepArgs("echovr.exe [-server] -fixedtimestep").removed.size(), 0u);
  // A quoted "-server" is a parameter to the game as well: not a server here (boot.cpp counts it one).
  EXPECT_FALSE(HasServerToken("echovr.exe \"-server\" -fixedtimestep"));
  // A trailing backslash before a quote: the game's quote ends at the next quote, with no escape, so the
  // `-server` after it is an option and the strip applies (CommandLineToArgvW would read `\"` as a quote).
  const auto r = StripServerTimestepArgs("echovr.exe -config \"C:\\dir\\\" -server -fixedtimestep");
  EXPECT_EQ(r.removed.size(), 1u);
  EXPECT_EQ(r.commandLine, "echovr.exe -config \"C:\\dir\\\" -server");
}

TEST(CmdlineStrip, ACommandLineWithNothingToRemoveIsReturnedAsIs) {
  const std::string text = "echovr.exe   -server   -noconsole ";
  const auto r = StripServerTimestepArgs(text);
  EXPECT_EQ(r.commandLine, text);
  EXPECT_TRUE(r.removed.empty());
}

// --- N1: removal must not glue the kept text into one token ---------------------------------------------

TEST(CmdlineStrip, ARemovalDoesNotGlueTheNextTokenOnToTheLast) {
  // "120" ends where -noconsole begins: without a space the result would read `-server-noconsole`.
  const auto r = StripServerTimestepArgs("echovr.exe -server -timestep \"120\"-noconsole");
  EXPECT_EQ(r.commandLine, "echovr.exe -server -noconsole");
  EXPECT_TRUE(Has(TextsOf(r.commandLine), "-noconsole"));
  // The same where the removed token is the first thing after the program name.
  const auto q = StripServerTimestepArgs("echovr.exe -server -timestep \"-server\"\"x + -FixedTimeStep  ");
  EXPECT_FALSE(HasTimestepOption(q.commandLine));
}

TEST(CmdlineStrip, TheResidualCasesFromTheReviewLeaveNoTimestepOption) {
  // Each is a line from the reviewer's probe (probe-residual.out), given a real -server option: before the
  // spacing rule the result still held a time-step option.
  const std::vector<std::string> inputs = {
      "echovr.exe -timestep \"-server\"\"x + -FixedTimeStep  ",
      "echovr.exe   -timestep     \"-server\"\" -timestep    ",
      "echovr.exe --timestep \"-server\"\"120 -fixedtimestep ",
      "echovr.exe -timestep [-server][ -timestep -5 ] [-server]",
      "echovr.exe --timestep    [-server]\"+ -FixedTimeStep",
      "echovr.exe -timestep \"-server\"\"-server 120-noconsole -fixedtimestep",
      "echovr.exe -timestep [-server]\"-noconsole \t-FixedTimeStep []",
  };
  for (const std::string& probe : inputs) {
    const std::string in = "echovr.exe -server " + probe.substr(std::string("echovr.exe ").size());
    ASSERT_TRUE(HasServerToken(in)) << in;
    const auto r = StripServerTimestepArgs(in);
    EXPECT_FALSE(HasTimestepOption(r.commandLine)) << "[" << in << "] -> [" << r.commandLine << "]";
  }
}

TEST(CmdlineStrip, EveryGeneratedServerLineEndsWithNoTimestepOptionAndStaysAServer) {
  // A deterministic fuzz over the pieces the game's tokenizer treats specially. For each line that has a
  // -server option: no time-step option is left, it is still a server, and a second strip changes nothing.
  // For a line without one: the text is returned untouched.
  const std::vector<std::string> pieces = {
      "echovr.exe", "-server", "-servers", "-timestep", "--timestep", "+TimeStep", "-FixedTimeStep", "-fixedtimestep",
      "-timestepper", "-config", "-noconsole", "120", "-5", "x", "\"", "[", "]", " ", "  ", "\t", "-", "+",
      std::string("\x96"), std::string("\x96") + "timestep", "\"-server\"", "[-server]", "\"120\"", "a b"};
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  auto next = [&state]() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::size_t>(state >> 33);
  };
  unsigned servers = 0;
  for (int iter = 0; iter < 20000; ++iter) {
    std::string line;
    const std::size_t count = 1 + next() % 9;
    for (std::size_t k = 0; k < count; ++k) {
      line += pieces[next() % pieces.size()];
      if (next() % 4 != 0) line += ' ';
    }
    const auto r = StripServerTimestepArgs(line);
    if (!HasServerToken(line)) {
      ASSERT_EQ(r.commandLine, line);
      continue;
    }
    ++servers;
    ASSERT_FALSE(HasTimestepOption(r.commandLine)) << "[" << line << "] -> [" << r.commandLine << "]";
    ASSERT_TRUE(HasServerToken(r.commandLine)) << "[" << line << "] -> [" << r.commandLine << "]";
    ASSERT_EQ(StripServerTimestepArgs(r.commandLine).commandLine, r.commandLine) << "[" << line << "]";
  }
  EXPECT_GT(servers, 1000u) << "the generator must produce server lines";
}

// --- N2: the values the game attaches to a removed option go with it ---------------------------------------

TEST(CmdlineStrip, EveryParameterTheGameWouldAttachToARemovedOptionGoesWithIt) {
  // The game attaches every non-option token after an option to that option, so x.json is -fixedtimestep's.
  const auto a = StripServerTimestepArgs("echovr.exe -server -config c.json -fixedtimestep x.json");
  EXPECT_EQ(a.commandLine, "echovr.exe -server -config c.json");
  EXPECT_EQ(a.removed.size(), 2u);
  // Several parameters: all of them, up to the next option.
  const auto b = StripServerTimestepArgs("echovr.exe -server -timestep 120 extra more -noconsole");
  EXPECT_EQ(b.commandLine, "echovr.exe -server -noconsole");
  // Parameters before the option stay with the option they follow.
  const auto c = StripServerTimestepArgs("echovr.exe -server -config c.json -timestep 120");
  EXPECT_EQ(c.commandLine, "echovr.exe -server -config c.json");
}

// --- N4: the tokenizer's branches ----------------------------------------------------------------------------

TEST(CmdlineStrip, ABracketedTokenSwallowsAnOptionInsideIt) {
  // `[x -fixedtimestep ]` is one token for the game: -fixedtimestep inside it is not an option.
  const std::string line = "echovr.exe -server -config [x -fixedtimestep ] -noconsole";
  const auto r = StripServerTimestepArgs(line);
  EXPECT_EQ(r.commandLine, line);
  EXPECT_TRUE(r.removed.empty());
}

TEST(CmdlineStrip, ABracketedTokenEndsAtItsClosingBracket) {
  // `[a]` closes at `]` (not at a quote), so the option after it is a real option and is removed.
  const auto r = StripServerTimestepArgs("echovr.exe -server -config [a] -fixedtimestep");
  EXPECT_EQ(r.commandLine, "echovr.exe -server -config [a]");
}

TEST(CmdlineStrip, AQuotedNameIsAParameterNotAnOption) {
  // "-fixedtimestep" in quotes is not an option to the game, so it is not removed, and it is -config's.
  const std::string line = "echovr.exe -server -config \"-fixedtimestep\" \"-timestep\"";
  const auto r = StripServerTimestepArgs(line);
  EXPECT_EQ(r.commandLine, line);
  EXPECT_TRUE(r.options.empty());
}

TEST(CmdlineStrip, AnyByteUpToSpaceSeparatesTokensButOnlyASpaceEndsOne) {
  // Between tokens the game skips every byte 1..0x20, a tab included, so a tab-separated -server is one.
  EXPECT_TRUE(HasServerToken("echovr.exe \t-server"));
  const auto r = StripServerTimestepArgs("echovr.exe \t-server\t -fixedtimestep");
  EXPECT_EQ(r.removed.size(), 0u) << "inside a token only a space ends it: `-server\\t` is not `-server`";
  const auto s = StripServerTimestepArgs("echovr.exe \t-server -fixedtimestep");
  EXPECT_EQ(s.commandLine, "echovr.exe \t-server");
}

// --- S1: the hook's one decision ------------------------------------------------------------------------------

TEST(PrepareForGame, AServerGetsANewPointerWithoutTheTokens) {
  const char* original = "echovr.exe -server -fixedtimestep -timestep 120 -noconsole";
  std::string storage;
  const auto h = PrepareForGame(original, storage);
  ASSERT_NE(h.commandLine, nullptr);
  EXPECT_NE(h.commandLine, original) << "a new pointer, not the original";
  EXPECT_EQ(h.commandLine, storage.c_str()) << "the text lives in the caller's storage";
  EXPECT_EQ(std::string(h.commandLine), "echovr.exe -server -noconsole");
  EXPECT_FALSE(HasTimestepOption(h.commandLine));
  EXPECT_EQ(h.tokensRemoved, 3u);
  EXPECT_EQ(h.options, (std::vector<std::string>{"-fixedtimestep", "-timestep"}));
  EXPECT_FALSE(h.residual);
}

TEST(PrepareForGame, AClientOrNothingToRemoveGetsTheOriginalPointer) {
  std::string storage = "untouched";
  const char* client = "echovr.exe -windowed -fixedtimestep -timestep 120";
  const auto c = PrepareForGame(client, storage);
  EXPECT_EQ(c.commandLine, client);
  EXPECT_EQ(c.tokensRemoved, 0u);
  EXPECT_TRUE(c.options.empty());
  const char* clean = "echovr.exe -server -noconsole";
  const auto s = PrepareForGame(clean, storage);
  EXPECT_EQ(s.commandLine, clean);
  EXPECT_EQ(s.tokensRemoved, 0u);
  EXPECT_EQ(storage, "untouched") << "nothing is written to the storage when nothing is removed";
}

TEST(PrepareForGame, ANullStringStaysNull) {
  std::string storage;
  const auto h = PrepareForGame(nullptr, storage);
  EXPECT_EQ(h.commandLine, nullptr);
  EXPECT_EQ(h.tokensRemoved, 0u);
}

TEST(PrepareForGame, TheReportNamesEveryRemovedOptionAndNeverLeavesAResidual) {
  std::string storage;
  const auto h = PrepareForGame("echovr.exe -server -FixedTimeStep --TimeStep 120", storage);
  EXPECT_EQ(h.tokensRemoved, 3u);
  EXPECT_EQ(h.options, (std::vector<std::string>{"-fixedtimestep", "-timestep"}));
  EXPECT_FALSE(h.residual);
}

// --- the server's step ---------------------------------------------------------------------------------------

TEST(CmdlineStrip, TheServerStepAfterTheStripIsTheConfiguredRate) {
  // The step NEVR sets once the arguments are gone: 1,000,000 / g_headlessTickRateHz. The rate defaults to
  // 120 Hz (core/globals.cpp), which is 8333 us.
  EXPECT_EQ(g_headlessTickRateHz, 120u);
  EXPECT_EQ(nevr_server_timestep::StepMicroseconds(g_headlessTickRateHz), 8333u);
}

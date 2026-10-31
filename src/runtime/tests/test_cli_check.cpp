// #1253: the command-line refusals that need no game state (lifecycle/cli_check.h). A refused line ends the run with
// exit code 64 through RejectCommandLine; these tests pin which lines are refused, which token is named, and which
// lines a run must still accept (a native game flag NEVR's table does not list, a server with a window flag absent).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "runtime/lifecycle/cli_check.h"

namespace {

// A command line as the wide tokens CommandLineToArgvW returns: the program path first, then the arguments.
struct Line {
  std::vector<std::wstring> tokens{L"echovr.exe"};

  Line& Add(const wchar_t* token) {
    tokens.push_back(token);
    return *this;
  }

  bool Check(nevr_cli_check::Rejection* out) {
    std::vector<wchar_t*> argv;
    argv.reserve(tokens.size());
    for (std::wstring& token : tokens) argv.push_back(token.data());
    return nevr_cli_check::Check(static_cast<int>(argv.size()), argv.data(), out);
  }
};

}  // namespace

TEST(CliCheck, AServerLineWithNoRefusedTokenPasses) {
  nevr_cli_check::Rejection out;
  EXPECT_FALSE(Line().Add(L"-server").Add(L"-headless").Add(L"-no-console").Check(&out));
}

TEST(CliCheck, ANativeGameFlagNevrDoesNotListIsNotRefused) {
  // -novr is a legal game token (the desktop client's launch flag), not in NEVR's table: it is not an error.
  nevr_cli_check::Rejection out;
  EXPECT_FALSE(Line().Add(L"-novr").Add(L"-windowed").Check(&out));
}

TEST(CliCheck, TheProgramPathIsNeverChecked) {
  nevr_cli_check::Rejection out;
  Line line;
  line.tokens[0] = L"-offline";
  EXPECT_FALSE(line.Add(L"-server").Check(&out));
}

TEST(CliCheck, ThreeLeadingDashesNamesTheTokenIndex) {
  nevr_cli_check::Rejection out;
  ASSERT_TRUE(Line().Add(L"-server").Add(L"---server").Check(&out));
  EXPECT_EQ(out.index, 2);
}

TEST(CliCheck, AValueFlagAtTheEndOfTheLineIsRefused) {
  nevr_cli_check::Rejection out;
  ASSERT_TRUE(Line().Add(L"-server").Add(L"-region").Check(&out));
  EXPECT_EQ(out.index, 2);
  EXPECT_NE(out.why.find(L"takes 1 argument"), std::wstring::npos);
}

TEST(CliCheck, AValueFlagFollowedByAnOptionIsRefusedAtTheFlag) {
  nevr_cli_check::Rejection out;
  ASSERT_TRUE(Line().Add(L"-config").Add(L"-server").Check(&out));
  EXPECT_EQ(out.index, 1);
}

TEST(CliCheck, AliasWithNoValueIsRefusedToo) {
  nevr_cli_check::Rejection out;
  ASSERT_TRUE(Line().Add(L"-server").Add(L"-telemetryrate").Check(&out));
  EXPECT_EQ(out.index, 2);
}

TEST(CliCheck, ValueFlagWithAValueIsAccepted) {
  nevr_cli_check::Rejection out;
  EXPECT_FALSE(Line().Add(L"-server").Add(L"-region").Add(L"us-east").Check(&out));
  EXPECT_FALSE(Line().Add(L"-config-path").Add(L"x.yaml").Check(&out));
}

TEST(CliCheck, TelemetryRateMustBeANumberAboveZero) {
  nevr_cli_check::Rejection out;
  ASSERT_TRUE(Line().Add(L"-server").Add(L"-telemetry-rate").Add(L"abc").Check(&out));
  EXPECT_EQ(out.index, 1);
  EXPECT_NE(out.why.find(L"abc"), std::wstring::npos);

  nevr_cli_check::Rejection zero;
  EXPECT_TRUE(Line().Add(L"-telemetry-rate").Add(L"0").Check(&zero));
  // "-5" is a value, not a flag, by the game's rule (cli_option_value.h), and it is not a number above zero.
  nevr_cli_check::Rejection negative;
  ASSERT_TRUE(Line().Add(L"-telemetry-rate").Add(L"-5").Check(&negative));
  EXPECT_EQ(negative.index, 1);
  nevr_cli_check::Rejection ten;
  EXPECT_FALSE(Line().Add(L"-telemetry-rate").Add(L"10").Check(&ten));
}

TEST(CliCheck, ServerWithOfflineIsRefusedAtTheOfflineToken) {
  nevr_cli_check::Rejection out;
  ASSERT_TRUE(Line().Add(L"-server").Add(L"-offline").Check(&out));
  EXPECT_EQ(out.index, 2);
  EXPECT_NE(out.why.find(L"mutually exclusive"), std::wstring::npos);
}

TEST(CliCheck, OfflineWithoutServerIsAccepted) {
  nevr_cli_check::Rejection out;
  EXPECT_FALSE(Line().Add(L"-offline").Check(&out));
}

TEST(CliCheck, ServerWithWindowedIsRefusedAtTheWindowedToken) {
  nevr_cli_check::Rejection out;
  ASSERT_TRUE(Line().Add(L"-server").Add(L"-windowed").Add(L"-no-console").Check(&out));
  EXPECT_EQ(out.index, 2);
}

TEST(CliCheck, ServerAfterWindowedIsStillRefused) {
  nevr_cli_check::Rejection out;
  ASSERT_TRUE(Line().Add(L"-windowed").Add(L"-server").Check(&out));
  EXPECT_EQ(out.index, 1);
}

TEST(CliCheck, WindowedWithoutServerIsAccepted) {
  nevr_cli_check::Rejection out;
  EXPECT_FALSE(Line().Add(L"-windowed").Check(&out));
}

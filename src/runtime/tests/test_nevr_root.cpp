// #857: the nevr root's paths (core/nevr_root.h) and the one-time move into it (lifecycle/nevr_root_move.cpp), driven for real
// against a temp tree. Built into test_nevr_root; run under Wine by `just test-auth-unit`.

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/nevr_root.h"
#include "runtime/lifecycle/nevr_root_move.h"

namespace {

namespace move = nevr_root_move;
using Outcome = move::Outcome;

std::string ReadAll(const std::wstring& path) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void Write(const std::wstring& path, const std::string& text) {
  ASSERT_TRUE(nevr_root::EnsureDirectoryTree(path.substr(0, path.find_last_of(L'\\'))));
  std::ofstream out(std::filesystem::path(path), std::ios::binary);
  out << text;
  ASSERT_TRUE(out.good()) << "could not write a fixture file";
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

unsigned Count(const std::vector<move::Event>& events, Outcome outcome) {
  unsigned n = 0;
  for (const move::Event& e : events) n += e.outcome == outcome ? 1 : 0;
  return n;
}

// A fresh tree per test, UTF-16 on purpose ("é", a CJK character): the user name in %LOCALAPPDATA% may not be ASCII.
//   <base>\game\bin\win10\_local   the install's _local, which the move must leave alone (install_local_)
//   <base>\appdata\EchoVR\...      the original game's folder, where nEVR used to write logs and cosmetic packages
//   <base>\appdata\nevr\echovr     the nevr root
class NevrRootMove : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    base_ = std::wstring(tmp) + L"nevr-root-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::wstring(test.begin(), test.end()) + L"-\u00e9\u4e2d";
    Wipe();
    install_local_ = base_ + L"\\game\\bin\\win10\\_local";
    legacy_logs_ = base_ + L"\\appdata\\EchoVR\\logs";
    legacy_cosmetics_ = base_ + L"\\appdata\\EchoVR\\cosmetics\\v1\\packages";
    root_ = nevr_root::RootUnder(base_ + L"\\appdata");
  }
  void TearDown() override { Wipe(); }

  void Wipe() {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(base_), ec);
  }

  move::Inputs Inputs() const {
    move::Inputs in;
    in.root = root_;
    in.legacy_logs_dir = legacy_logs_;
    in.legacy_cosmetics_dir = legacy_cosmetics_;
    return in;
  }

  // The layout an older release left behind.
  void OldLayout() {
    Write(legacy_logs_ + L"\\nevr-2026-10-01T10-00-00.000.jsonl", "{\"msg\":\"run log\"}\n");
    Write(legacy_logs_ + L"\\nevr-2026-10-01T10-00-00.001.log", "plain run log\n");
    Write(legacy_logs_ + L"\\nevr-crash-abc123.txt", "[NEVR.CRASH] record\n");
    Write(legacy_logs_ + L"\\other-tool.log", "not ours\n");
    Write(legacy_cosmetics_ + L"\\hat.evrp", "package bytes");
  }

  std::wstring base_, root_, install_local_, legacy_logs_, legacy_cosmetics_;
};

TEST(NevrRootPaths, EveryFolderIsUnderTheRootUnderLocalAppData) {
  const std::wstring root = nevr_root::RootUnder(std::wstring(L"C:\\Users\\u\\AppData\\Local"));
  EXPECT_EQ(root, L"C:\\Users\\u\\AppData\\Local\\nevr\\echovr");
  EXPECT_EQ(nevr_root::LogsDir(root), root + L"\\logs");
  EXPECT_EQ(nevr_root::CrashesDir(root), root + L"\\crashes");
  EXPECT_EQ(nevr_root::CacheDir(root), root + L"\\cache");
  EXPECT_EQ(nevr_root::CosmeticsDir(root), root + L"\\cosmetics\\v1\\packages");
  EXPECT_EQ(nevr_root::RootUnder(std::string("C:\\L\\")), "C:\\L\\nevr\\echovr") << "one separator, however the base ends";
}

TEST(NevrRootPaths, AnUnresolvedRootStaysUnresolvedAllTheWayDown) {
  const std::wstring none = nevr_root::RootUnder(std::wstring());
  EXPECT_TRUE(none.empty());
  EXPECT_TRUE(nevr_root::LogsDir(none).empty());
  EXPECT_TRUE(nevr_root::CosmeticsDir(none).empty());
}

TEST(NevrRootPaths, RootWReadsLocalAppDataAndNothingElse) {
  wchar_t saved[MAX_PATH * 2] = {};
  const DWORD had = GetEnvironmentVariableW(L"LOCALAPPDATA", saved, static_cast<DWORD>(sizeof(saved) / sizeof(saved[0])));
  SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\\u00e9\u4e2d\\AppData\\Local");
  EXPECT_EQ(nevr_root::RootW(), L"C:\\Users\\\u00e9\u4e2d\\AppData\\Local\\nevr\\echovr");
  EXPECT_EQ(nevr_root::RootA().empty(), false);
  SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  EXPECT_TRUE(nevr_root::RootW().empty());
  EXPECT_TRUE(nevr_root::RootA().empty());
  if (had != 0) SetEnvironmentVariableW(L"LOCALAPPDATA", saved);
}

TEST(NevrRootPaths, TheNamesNEVRWritesAreRecognisedAndNothingElseIs) {
  EXPECT_TRUE(move::IsNevrLogFileName(L"nevr-2026-10-01T10-00-00.000.jsonl"));
  EXPECT_TRUE(move::IsNevrLogFileName(L"nevr-2026-10-01T10-00-00.000.log"));
  EXPECT_TRUE(move::IsNevrLogFileName(L"nevr-crash-abc123.txt"));
  EXPECT_FALSE(move::IsNevrLogFileName(L"nevr-crash-abc123.jsonl")) << "the crash record is .txt";
  EXPECT_FALSE(move::IsNevrLogFileName(L"nevr.jsonl"));
  EXPECT_FALSE(move::IsNevrLogFileName(L"other-tool.log"));
  EXPECT_FALSE(move::IsNevrLogFileName(L"MOVED-TO-NEVR-ROOT.txt"));
  EXPECT_FALSE(move::IsNevrLogFileName(L"nevr-notes.txt"));
}

TEST(NevrRootPaths, EnsureDirectoryTreeMakesEveryParentAndRefusesAFile) {
  wchar_t tmp[MAX_PATH];
  ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
  const std::wstring base = std::wstring(tmp) + L"nevr-root-test-tree-" + std::to_wstring(GetCurrentProcessId());
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(base), ec);
  EXPECT_TRUE(nevr_root::EnsureDirectoryTree(base + L"\\a\\b\\c"));
  EXPECT_TRUE(Exists(base + L"\\a\\b\\c"));
  EXPECT_TRUE(nevr_root::EnsureDirectoryTree(base + L"\\a\\b\\c")) << "an existing tree is fine";
  Write(base + L"\\file", "x");
  EXPECT_FALSE(nevr_root::EnsureDirectoryTree(base + L"\\file\\child")) << "a file where a folder is needed";
  EXPECT_FALSE(nevr_root::EnsureDirectoryTree(std::wstring()));
  std::filesystem::remove_all(std::filesystem::path(base), ec);
}

TEST_F(NevrRootMove, TheOldLayoutMovesIntoTheRootAndEachEmptiedFolderSaysWhy) {
  OldLayout();
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(Count(events, Outcome::kMoved), 4u) << "three nEVR log files, one cosmetic package";
  EXPECT_EQ(Count(events, Outcome::kMarkerWritten), 2u) << "the old logs folder and the old cosmetics folder";
  EXPECT_EQ(ReadAll(root_ + L"\\logs\\nevr-2026-10-01T10-00-00.000.jsonl"), "{\"msg\":\"run log\"}\n");
  EXPECT_EQ(ReadAll(root_ + L"\\logs\\nevr-2026-10-01T10-00-00.001.log"), "plain run log\n");
  EXPECT_EQ(ReadAll(root_ + L"\\logs\\nevr-crash-abc123.txt"), "[NEVR.CRASH] record\n");
  EXPECT_EQ(ReadAll(root_ + L"\\cosmetics\\v1\\packages\\hat.evrp"), "package bytes");

  // The old copies are gone: nothing reads them any more.
  EXPECT_FALSE(Exists(legacy_logs_ + L"\\nevr-crash-abc123.txt"));
  EXPECT_FALSE(Exists(legacy_cosmetics_ + L"\\hat.evrp"));
  // What is not nEVR's stays.
  EXPECT_EQ(ReadAll(legacy_logs_ + L"\\other-tool.log"), "not ours\n");

  for (const std::wstring& dir : {legacy_logs_, legacy_cosmetics_}) {
    const std::string marker = ReadAll(dir + L"\\MOVED-TO-NEVR-ROOT.txt");
    EXPECT_NE(marker.find("nevr\\echovr"), std::string::npos) << "the marker names the root";
  }
  EXPECT_FALSE(Exists(install_local_ + L"\\MOVED-TO-NEVR-ROOT.txt")) << "a folder nothing was taken from gets no marker";
}

TEST_F(NevrRootMove, ASecondRunMovesNothing) {
  OldLayout();
  ASSERT_FALSE(move::Run(Inputs()).empty());
  const std::string marker = ReadAll(legacy_logs_ + L"\\MOVED-TO-NEVR-ROOT.txt");
  const std::vector<move::Event> again = move::Run(Inputs());
  EXPECT_TRUE(again.empty()) << "no file is left to move, so no event and no rewritten marker";
  EXPECT_EQ(ReadAll(legacy_logs_ + L"\\MOVED-TO-NEVR-ROOT.txt"), marker);
  EXPECT_EQ(ReadAll(root_ + L"\\logs\\nevr-crash-abc123.txt"), "[NEVR.CRASH] record\n");
}

TEST_F(NevrRootMove, NothingToMoveCreatesNoRoot) {
  EXPECT_TRUE(move::Run(Inputs()).empty());
  EXPECT_FALSE(Exists(root_));
}

TEST_F(NevrRootMove, TheMoveNeverOverwritesAFileAlreadyInTheRoot) {
  Write(root_ + L"\\logs\\nevr-crash-abc123.txt", "root copy");
  Write(legacy_logs_ + L"\\nevr-crash-abc123.txt", "old copy");
  const std::vector<move::Event> events = move::Run(Inputs());

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].outcome, Outcome::kLeftRootHasIt);
  EXPECT_EQ(ReadAll(root_ + L"\\logs\\nevr-crash-abc123.txt"), "root copy");
  EXPECT_EQ(ReadAll(legacy_logs_ + L"\\nevr-crash-abc123.txt"), "old copy") << "nothing is deleted that was not moved";
  EXPECT_FALSE(Exists(legacy_logs_ + L"\\MOVED-TO-NEVR-ROOT.txt")) << "nothing was moved, so no marker";
}

TEST_F(NevrRootMove, AFileThatCannotBeMovedStaysAndTheNextRunMovesIt) {
  const std::wstring log = legacy_logs_ + L"\\nevr-2026-10-01T10-00-00.000.jsonl";
  Write(log, "run log");
  HANDLE hold = CreateFileW(log.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(hold, INVALID_HANDLE_VALUE);

  const std::vector<move::Event> events = move::Run(Inputs());
  CloseHandle(hold);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].outcome, Outcome::kLeftMoveFailed);
  EXPECT_NE(events[0].error, 0ul);
  EXPECT_EQ(ReadAll(log), "run log");
  EXPECT_FALSE(Exists(root_ + L"\\logs\\nevr-2026-10-01T10-00-00.000.jsonl"));
  EXPECT_FALSE(Exists(legacy_logs_ + L"\\MOVED-TO-NEVR-ROOT.txt")) << "the marker says files moved; none did";

  EXPECT_EQ(Count(move::Run(Inputs()), Outcome::kMoved), 1u);
  EXPECT_EQ(ReadAll(root_ + L"\\logs\\nevr-2026-10-01T10-00-00.000.jsonl"), "run log");
}

TEST_F(NevrRootMove, ARootThatCannotBeCreatedMovesNothingAndSaysSo) {
  Write(legacy_logs_ + L"\\nevr-crash-abc123.txt", "[NEVR.CRASH] record\n");
  Write(base_ + L"\\appdata\\nevr", "a file where the root's parent folder belongs");
  const std::vector<move::Event> events = move::Run(Inputs());

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].outcome, Outcome::kRootUnavailable);
  EXPECT_EQ(ReadAll(legacy_logs_ + L"\\nevr-crash-abc123.txt"), "[NEVR.CRASH] record\n");
}

TEST_F(NevrRootMove, WithoutARootTheMoveDoesNothing) {
  OldLayout();
  move::Inputs in = Inputs();
  in.root.clear();
  EXPECT_TRUE(move::Run(in).empty());
  EXPECT_TRUE(Exists(legacy_logs_ + L"\\nevr-crash-abc123.txt"));
}

TEST_F(NevrRootMove, NothingInTheInstallsLocalFolderIsMoved) {
  Write(install_local_ + L"\\config.yaml", "services: near");
  Write(install_local_ + L"\\.credentials.json", "{\"refresh_token\":\"x\"}");
  OldLayout();
  move::Run(Inputs());
  EXPECT_EQ(ReadAll(install_local_ + L"\\config.yaml"), "services: near") << "config.yaml keeps its resolution (ADR 0012 step 6)";
  EXPECT_TRUE(Exists(install_local_ + L"\\.credentials.json"));
  EXPECT_FALSE(Exists(root_ + L"\\config.yaml"));
  EXPECT_FALSE(Exists(root_ + L"\\.credentials.json"));
  EXPECT_FALSE(Exists(install_local_ + L"\\MOVED-TO-NEVR-ROOT.txt"));
}

TEST(NevrRootMoveEnvironment, TheInputsComeFromLocalAppData) {
  wchar_t saved[MAX_PATH * 2] = {};
  const DWORD had = GetEnvironmentVariableW(L"LOCALAPPDATA", saved, static_cast<DWORD>(sizeof(saved) / sizeof(saved[0])));
  SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\u\\AppData\\Local");
  const move::Inputs in = move::InputsFromEnvironment();
  EXPECT_EQ(in.root, L"C:\\Users\\u\\AppData\\Local\\nevr\\echovr");
  EXPECT_EQ(in.legacy_logs_dir, L"C:\\Users\\u\\AppData\\Local\\EchoVR\\logs");
  EXPECT_EQ(in.legacy_cosmetics_dir, L"C:\\Users\\u\\AppData\\Local\\EchoVR\\cosmetics\\v1\\packages");
  SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  EXPECT_TRUE(move::InputsFromEnvironment().root.empty());
  if (had != 0) SetEnvironmentVariableW(L"LOCALAPPDATA", saved);
}

}  // namespace

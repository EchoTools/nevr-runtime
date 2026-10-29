// #857: the nevr root's paths (core/nevr_root.h) and the one-time move into it (lifecycle/nevr_root_move.cpp), driven for real
// against a temp tree. Built into test_nevr_root; run under Wine by `just test-auth-unit`.

#include <gtest/gtest.h>

#include <winsock2.h>  // before windows.h: core/credential_write.h reaches core/logging.h, which pulls it in
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/credential_write.h"
#include "core/nevr_root.h"
#include "core/system_info.h"
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
//   <base>\game\bin\win10\_local   the install's nearest _local        (local_[0], install_local_)
//   <base>\game\bin\_local         one parent up                       (local_[1])
//   <base>\game\_local             two parents up                      (local_[2])
//   <base>\game\bin\win10\plugins  where plugins used to be loaded from    (legacy_plugins_)
//   <base>\appdata\EchoVR\...      the original game's folder, where nEVR used to write logs and cosmetic packages
//   <base>\appdata\nevr\echovr     the nevr root
class NevrRootMove : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    base_ = std::wstring(tmp) + L"nevr-root-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::wstring(test.begin(), test.end()) + Suffix();
    Wipe();
    install_local_ = base_ + L"\\game\\bin\\win10\\_local";
    local_ = {install_local_, base_ + L"\\game\\bin\\_local", base_ + L"\\game\\_local"};
    legacy_logs_ = base_ + L"\\appdata\\EchoVR\\logs";
    legacy_cosmetics_ = base_ + L"\\appdata\\EchoVR\\cosmetics\\v1\\packages";
    legacy_plugins_ = base_ + L"\\game\\bin\\win10\\plugins";
    root_ = nevr_root::RootUnder(base_ + L"\\appdata");
  }
  void TearDown() override { Wipe(); }
  // UTF-16 on purpose; the sign-in cache's tests use plain ASCII (the credential writer takes ANSI paths).
  virtual std::wstring Suffix() const { return L"-\u00e9\u4e2d"; }

  void Wipe() {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(base_), ec);
  }

  move::Inputs Inputs() const {
    move::Inputs in;
    in.root = root_;
    in.local_dirs = local_;
    in.legacy_logs_dir = legacy_logs_;
    in.legacy_cosmetics_dir = legacy_cosmetics_;
    in.legacy_plugins_dir = legacy_plugins_;
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

  std::wstring base_, root_, install_local_, legacy_logs_, legacy_cosmetics_, legacy_plugins_;
  std::vector<std::wstring> local_;
};

TEST(NevrRootPaths, EveryFolderIsUnderTheRootUnderLocalAppData) {
  const std::wstring root = nevr_root::RootUnder(std::wstring(L"C:\\Users\\u\\AppData\\Local"));
  EXPECT_EQ(root, L"C:\\Users\\u\\AppData\\Local\\nevr\\echovr");
  EXPECT_EQ(nevr_root::LogsDir(root), root + L"\\logs");
  EXPECT_EQ(nevr_root::CrashesDir(root), root + L"\\crashes");
  EXPECT_EQ(nevr_root::CacheDir(root), root + L"\\cache");
  EXPECT_EQ(nevr_root::CosmeticsDir(root), root + L"\\cosmetics\\v1\\packages");
  EXPECT_EQ(nevr_root::PluginsDir(root), root + L"\\plugins");
  EXPECT_EQ(nevr_root::PluginsDir(std::string("C:\\L\\nevr\\echovr")), "C:\\L\\nevr\\echovr\\plugins");
  EXPECT_EQ(nevr_root::RootUnder(std::string("C:\\L\\")), "C:\\L\\nevr\\echovr") << "one separator, however the base ends";
}

TEST(NevrRootPaths, AnUnresolvedRootStaysUnresolvedAllTheWayDown) {
  const std::wstring none = nevr_root::RootUnder(std::wstring());
  EXPECT_TRUE(none.empty());
  EXPECT_TRUE(nevr_root::LogsDir(none).empty());
  EXPECT_TRUE(nevr_root::CosmeticsDir(none).empty());
  EXPECT_TRUE(nevr_root::PluginsDir(none).empty());
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

// #856: the plugins folder beside echovr.exe moves into the root's plugins\ (ADR 0012).
TEST_F(NevrRootMove, ThePluginsFolderBesideTheGameMovesIntoTheRootsPluginsAndSaysWhy) {
  Write(legacy_plugins_ + L"\\alpha.dll", "alpha bytes");
  Write(legacy_plugins_ + L"\\beta.dll", "beta bytes");
  Write(legacy_plugins_ + L"\\beta.yaml", "beta data");
  Write(legacy_plugins_ + L"\\sub\\gamma.dll", "in a subfolder");
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(Count(events, Outcome::kMoved), 3u) << "the three files at the top of the folder";
  EXPECT_EQ(Count(events, Outcome::kMarkerWritten), 0u) << "nothing of ours is written to the game folder";
  EXPECT_EQ(ReadAll(root_ + L"\\plugins\\alpha.dll"), "alpha bytes");
  EXPECT_EQ(ReadAll(root_ + L"\\plugins\\beta.dll"), "beta bytes");
  EXPECT_EQ(ReadAll(root_ + L"\\plugins\\beta.yaml"), "beta data");
  EXPECT_FALSE(Exists(legacy_plugins_ + L"\\alpha.dll")) << "the old copy is gone: nothing loads from there any more";
  EXPECT_FALSE(Exists(legacy_plugins_ + L"\\beta.dll"));
  EXPECT_EQ(ReadAll(legacy_plugins_ + L"\\sub\\gamma.dll"), "in a subfolder") << "a folder is not a plugin file and is left alone";
  EXPECT_FALSE(Exists(root_ + L"\\plugins\\sub"));
  EXPECT_FALSE(Exists(legacy_plugins_ + L"\\MOVED-TO-NEVR-ROOT.txt")) << "no note is left in the game folder";
}

TEST_F(NevrRootMove, APluginAlreadyInTheRootIsNeverOverwritten) {
  Write(root_ + L"\\plugins\\alpha.dll", "root copy");
  Write(legacy_plugins_ + L"\\alpha.dll", "old copy");
  Write(legacy_plugins_ + L"\\beta.dll", "beta bytes");
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(Count(events, Outcome::kLeftRootHasIt), 1u);
  EXPECT_EQ(Count(events, Outcome::kMoved), 1u);
  EXPECT_EQ(ReadAll(root_ + L"\\plugins\\alpha.dll"), "root copy");
  EXPECT_EQ(ReadAll(legacy_plugins_ + L"\\alpha.dll"), "old copy") << "nothing is deleted that was not moved";
  EXPECT_EQ(ReadAll(root_ + L"\\plugins\\beta.dll"), "beta bytes");
}

TEST_F(NevrRootMove, ASecondPluginsRunMovesNothingAndLeavesNothingBehind) {
  Write(legacy_plugins_ + L"\\alpha.dll", "alpha bytes");
  ASSERT_EQ(Count(move::Run(Inputs()), Outcome::kMoved), 1u);
  EXPECT_FALSE(Exists(legacy_plugins_ + L"\\MOVED-TO-NEVR-ROOT.txt")) << "the first run left no note in the game folder";
  EXPECT_TRUE(move::Run(Inputs()).empty()) << "nothing is left to move";
  EXPECT_FALSE(Exists(root_ + L"\\plugins\\MOVED-TO-NEVR-ROOT.txt"));
  Write(legacy_plugins_ + L"\\delta.dll", "a later plugin");
  EXPECT_EQ(Count(move::Run(Inputs()), Outcome::kMoved), 1u) << "a plugin dropped there later is moved too";
  EXPECT_EQ(ReadAll(root_ + L"\\plugins\\delta.dll"), "a later plugin");
}

TEST_F(NevrRootMove, PluginsStayWhenTheRootsPluginsFolderCannotBeMade) {
  Write(root_ + L"\\plugins", "a file where the plugins folder belongs");
  Write(legacy_plugins_ + L"\\alpha.dll", "alpha bytes");
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(Count(events, Outcome::kLeftMoveFailed), 1u);
  EXPECT_EQ(Count(events, Outcome::kMoved), 0u);
  EXPECT_EQ(ReadAll(legacy_plugins_ + L"\\alpha.dll"), "alpha bytes");
  EXPECT_FALSE(Exists(legacy_plugins_ + L"\\MOVED-TO-NEVR-ROOT.txt")) << "nothing moved, so no marker";
}

// The sign-in cache moves through the protected write, whose paths are ANSI: these run under a plain ASCII folder name.
class NevrRootCredentialMove : public NevrRootMove {
 protected:
  std::wstring Suffix() const override { return L"-ascii"; }

  std::wstring Cache(const std::wstring& dir) const { return dir + L"\\.credentials.json"; }
  static std::string Narrow(const std::wstring& wide) { return std::string(wide.begin(), wide.end()); }
};

TEST_F(NevrRootCredentialMove, TheSignInCacheMovesIntoTheRootAndTheLocalFolderSaysWhy) {
  const std::string token = "{\"refresh_token\":\"r-1\",\"refresh_token_expiry\":2000000000}";
  Write(Cache(local_[0]), token);
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(Count(events, Outcome::kMoved), 1u);
  EXPECT_EQ(Count(events, Outcome::kMarkerWritten), 1u);
  EXPECT_EQ(ReadAll(Cache(root_)), token) << "byte for byte: the login survives";
  EXPECT_FALSE(Exists(Cache(local_[0])));
  EXPECT_FALSE(Exists(Cache(root_) + L".tmp")) << "the temp file of the protected write is renamed away";
  EXPECT_NE(ReadAll(local_[0] + L"\\MOVED-TO-NEVR-ROOT.txt").find("nevr\\echovr"), std::string::npos);
  if (nevr_system_info::Get().IsWine()) {
    EXPECT_EQ(nevr_credential_write::WineHostMode(Narrow(Cache(root_))), "600") << "as private as a cache saved by the runtime";
  }
}

TEST_F(NevrRootCredentialMove, TheMoveNeverOverwritesTheRootsCache) {
  Write(Cache(root_), "root cache");
  Write(Cache(local_[0]), "old cache");
  const std::vector<move::Event> events = move::Run(Inputs());

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].outcome, Outcome::kLeftRootHasIt);
  EXPECT_EQ(ReadAll(Cache(root_)), "root cache");
  EXPECT_EQ(ReadAll(Cache(local_[0])), "old cache");
  EXPECT_FALSE(Exists(local_[0] + L"\\MOVED-TO-NEVR-ROOT.txt"));
}

TEST_F(NevrRootCredentialMove, TheNearestCacheWinsAndTheFartherOnesAreLeft) {
  Write(Cache(local_[0]), "near");
  Write(Cache(local_[1]), "parent");
  Write(Cache(local_[2]), "grandparent");
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(ReadAll(Cache(root_)), "near");
  EXPECT_EQ(Count(events, Outcome::kMoved), 1u);
  EXPECT_EQ(Count(events, Outcome::kLeftRootHasIt), 2u);
  EXPECT_EQ(ReadAll(Cache(local_[1])), "parent");
  EXPECT_EQ(ReadAll(Cache(local_[2])), "grandparent");
}

TEST_F(NevrRootCredentialMove, ACacheThatCannotBeReadStaysAndNothingIsWrittenToTheRoot) {
  Write(Cache(local_[0]), "held");
  HANDLE hold = CreateFileW(Cache(local_[0]).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(hold, INVALID_HANDLE_VALUE);
  const std::vector<move::Event> events = move::Run(Inputs());
  CloseHandle(hold);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].outcome, Outcome::kLeftMoveFailed);
  EXPECT_NE(events[0].error, 0ul);
  EXPECT_EQ(ReadAll(Cache(local_[0])), "held");
  EXPECT_FALSE(Exists(Cache(root_))) << "a cache in the root outranks the old one, so a failed move leaves none";
  EXPECT_FALSE(Exists(local_[0] + L"\\MOVED-TO-NEVR-ROOT.txt"));

  EXPECT_EQ(Count(move::Run(Inputs()), Outcome::kMoved), 1u) << "the next run tries again";
  EXPECT_EQ(ReadAll(Cache(root_)), "held");
}

TEST_F(NevrRootCredentialMove, OnlyTheSignInCacheIsTakenFromTheInstallsLocalFolders) {
  Write(install_local_ + L"\\config.yaml", "services: near");
  Write(install_local_ + L"\\config.json", "{}");
  Write(Cache(install_local_), "cache");
  move::Run(Inputs());
  EXPECT_EQ(ReadAll(install_local_ + L"\\config.yaml"), "services: near") << "config.yaml keeps its resolution (ADR 0012 step 6)";
  EXPECT_EQ(ReadAll(install_local_ + L"\\config.json"), "{}");
  EXPECT_FALSE(Exists(root_ + L"\\config.yaml"));
  EXPECT_EQ(ReadAll(Cache(root_)), "cache");
}

TEST_F(NevrRootCredentialMove, ASecondRunMovesNothing) {
  Write(Cache(local_[0]), "cache");
  ASSERT_FALSE(move::Run(Inputs()).empty());
  EXPECT_TRUE(move::Run(Inputs()).empty());
  EXPECT_EQ(ReadAll(Cache(root_)), "cache");
}

// #1114: another process acting while this one moves the cache. Each hook below stands in for that process at one point of the
// move; the cache path the hooks write is set by the test.
namespace {
std::wstring g_hookCache;  // where the other process writes its sign-in
std::string g_hookBytes;
void OtherProcessWritesItsSignIn(const std::wstring&) {
  std::ofstream out(std::filesystem::path(g_hookCache), std::ios::binary | std::ios::trunc);
  out << g_hookBytes;
}
void OtherProcessWritesItsSignInAtTheRoot(const std::wstring& installed) { OtherProcessWritesItsSignIn(installed); }
void CorruptTheTemp(const std::wstring& temp) {
  std::ofstream out(std::filesystem::path(temp), std::ios::binary | std::ios::app);
  out << "x";
}
void WriteMidWrite() { OtherProcessWritesItsSignIn(g_hookCache); }
HANDLE g_holdsTheCache = INVALID_HANDLE_VALUE;
void SomethingHoldsTheCacheOpen(const std::wstring& installed) {
  g_holdsTheCache = CreateFileW(installed.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

std::vector<std::wstring> NamesIn(const std::wstring& dir) {
  std::vector<std::wstring> names;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dir), ec)) {
    names.push_back(entry.path().filename().wstring());
  }
  return names;
}
unsigned TempsIn(const std::wstring& dir) {
  unsigned n = 0;
  for (const std::wstring& name : NamesIn(dir)) n += name.size() > 4 && name.compare(name.size() - 4, 4, L".tmp") == 0 ? 1 : 0;
  return n;
}
}  // namespace

class NevrRootConcurrentMove : public NevrRootCredentialMove {
 protected:
  void SetUp() override {
    NevrRootCredentialMove::SetUp();
    g_hookCache = Cache(root_);
    g_hookBytes = "fresh sign-in";
  }
  void TearDown() override {
    nevr_credential_write::g_midWriteHook = nullptr;
    move::g_afterTempWrittenHook = nullptr;
    move::g_afterInstallHook = nullptr;
    NevrRootCredentialMove::TearDown();
  }
};

TEST(NevrRootTempName, TheMovesTempNameIsItsOwnPerProcessAndNewEachCall) {
  const std::string cache = "C:\\x\\.credentials.json";
  const std::string first = nevr_credential_write::UniqueTempPathFor(cache);
  const std::string second = nevr_credential_write::UniqueTempPathFor(cache);
  EXPECT_NE(first, nevr_credential_write::TempPathFor(cache)) << "the cache's own save uses that name";
  EXPECT_NE(first, second);
  const std::string pid = "." + std::to_string(GetCurrentProcessId()) + ".";
  EXPECT_NE(first.find(pid), std::string::npos) << first << ": a second process must get another name";
  EXPECT_EQ(first.rfind(cache + ".", 0), 0u) << "beside the cache, so the install is a rename on one volume";
  EXPECT_EQ(first.substr(first.size() - 4), ".tmp");
}

TEST_F(NevrRootConcurrentMove, ASignInWrittenWhileTheMoveIsInFlightIsNeverOverwritten) {
  Write(Cache(local_[0]), "old cache");
  nevr_credential_write::g_midWriteHook = &WriteMidWrite;  // after the root was checked, before the cache is installed
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(ReadAll(Cache(root_)), "fresh sign-in") << "the move replaced a sign-in it did not write";
  EXPECT_EQ(ReadAll(Cache(local_[0])), "old cache") << "the old cache is left where it is";
  EXPECT_EQ(Count(events, Outcome::kMoved), 0u);
  EXPECT_EQ(Count(events, Outcome::kLeftRootHasIt), 1u);
  EXPECT_EQ(TempsIn(root_), 0u) << "the move's temp file is removed when the root has the cache";
  EXPECT_FALSE(Exists(local_[0] + L"\\MOVED-TO-NEVR-ROOT.txt"));
}

TEST_F(NevrRootConcurrentMove, ASignInThatReplacesTheCopyBeforeTheReadBackIsNeverDeleted) {
  Write(Cache(local_[0]), "old cache");
  move::g_afterInstallHook = &OtherProcessWritesItsSignInAtTheRoot;
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(ReadAll(Cache(root_)), "fresh sign-in") << "the mismatch path deleted a sign-in the move did not write";
  EXPECT_EQ(ReadAll(Cache(local_[0])), "old cache");
  EXPECT_EQ(Count(events, Outcome::kMoved), 0u);
  EXPECT_FALSE(Exists(local_[0] + L"\\MOVED-TO-NEVR-ROOT.txt"));
}

TEST_F(NevrRootConcurrentMove, ACopyThatCannotBeReadBackKeepsTheOldCacheAndDeletesNothing) {
  Write(Cache(local_[0]), "old cache");
  move::g_afterInstallHook = &SomethingHoldsTheCacheOpen;
  const std::vector<move::Event> events = move::Run(Inputs());
  ASSERT_NE(g_holdsTheCache, INVALID_HANDLE_VALUE);
  CloseHandle(g_holdsTheCache);
  g_holdsTheCache = INVALID_HANDLE_VALUE;

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].outcome, Outcome::kLeftMoveFailed);
  EXPECT_NE(events[0].error, 0ul);
  EXPECT_EQ(ReadAll(Cache(local_[0])), "old cache") << "the old cache goes only once the copy was read back";
  EXPECT_EQ(ReadAll(Cache(root_)), "old cache") << "the copy that was installed is left in place";
  EXPECT_FALSE(Exists(local_[0] + L"\\MOVED-TO-NEVR-ROOT.txt"));
}

TEST_F(NevrRootConcurrentMove, AnotherWritersTempFileIsNeverTouched) {
  Write(Cache(local_[0]), "old cache");
  const std::wstring theirs = Cache(root_) + L".tmp";  // the name the sign-in cache's own save uses
  Write(theirs, "another process, mid-write");
  const std::vector<move::Event> events = move::Run(Inputs());

  EXPECT_EQ(Count(events, Outcome::kMoved), 1u);
  EXPECT_EQ(ReadAll(Cache(root_)), "old cache");
  EXPECT_EQ(ReadAll(theirs), "another process, mid-write") << "the move removed or reused a temp file it did not create";
  EXPECT_EQ(TempsIn(root_), 1u) << "only the other process's temp file is left";
}

TEST_F(NevrRootConcurrentMove, ACopyThatFailsItsCheckIsNeverInstalled) {
  Write(Cache(local_[0]), "old cache");
  move::g_afterTempWrittenHook = &CorruptTheTemp;
  const std::vector<move::Event> events = move::Run(Inputs());

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].outcome, Outcome::kLeftMoveFailed);
  EXPECT_FALSE(Exists(Cache(root_))) << "a cache in the root outranks the old one, so a failed copy leaves none";
  EXPECT_EQ(ReadAll(Cache(local_[0])), "old cache");
  EXPECT_EQ(TempsIn(root_), 0u);

  move::g_afterTempWrittenHook = nullptr;
  EXPECT_EQ(Count(move::Run(Inputs()), Outcome::kMoved), 1u) << "the next run tries again";
  EXPECT_EQ(ReadAll(Cache(root_)), "old cache");
}

TEST_F(NevrRootConcurrentMove, TwoMovesAtOnceInstallOneCacheAndLeaveTheOtherSource) {
  for (int round = 0; round < 5; ++round) {
    Wipe();
    Write(Cache(local_[0]), "a");
    Write(Cache(local_[1]), "b");
    move::Inputs a = Inputs();
    a.local_dirs = {local_[0]};
    move::Inputs b = Inputs();
    b.local_dirs = {local_[1]};
    std::vector<move::Event> ea, eb;
    std::atomic<bool> go{false};
    std::thread ta([&] { while (!go) {} ea = move::Run(a); });
    std::thread tb([&] { while (!go) {} eb = move::Run(b); });
    go = true;
    ta.join();
    tb.join();

    const unsigned moved = Count(ea, Outcome::kMoved) + Count(eb, Outcome::kMoved);
    const std::string root = ReadAll(Cache(root_));
    ASSERT_EQ(moved, 1u) << "round " << round << ": both moves reported installing the cache";
    ASSERT_TRUE(root == "a" || root == "b") << "round " << round << ": the root holds '" << root << "'";
    EXPECT_EQ(Exists(Cache(local_[0])), root != "a") << "the source of the winner goes, the other stays";
    EXPECT_EQ(Exists(Cache(local_[1])), root != "b");
    EXPECT_EQ(TempsIn(root_), 0u);
  }
}

TEST_F(NevrRootMove, ACachePathThatIsNotAnsiIsLeftNotLost) {
  if (GetACP() == CP_UTF8) GTEST_SKIP() << "the ANSI code page is UTF-8 here, so every path converts";
  Write(local_[0] + L"\\.credentials.json", "cache");
  const std::vector<move::Event> events = move::Run(Inputs());
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].outcome, Outcome::kLeftMoveFailed);
  EXPECT_EQ(events[0].error, static_cast<unsigned long>(ERROR_NO_UNICODE_TRANSLATION));
  EXPECT_EQ(ReadAll(local_[0] + L"\\.credentials.json"), "cache");
  EXPECT_FALSE(Exists(root_ + L"\\.credentials.json"));
}

TEST(NevrRootMoveEnvironment, TheInputsComeFromLocalAppData) {
  wchar_t saved[MAX_PATH * 2] = {};
  const DWORD had = GetEnvironmentVariableW(L"LOCALAPPDATA", saved, static_cast<DWORD>(sizeof(saved) / sizeof(saved[0])));
  SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\u\\AppData\\Local");
  const move::Inputs in = move::InputsFromEnvironment();
  EXPECT_EQ(in.root, L"C:\\Users\\u\\AppData\\Local\\nevr\\echovr");
  EXPECT_EQ(in.legacy_logs_dir, L"C:\\Users\\u\\AppData\\Local\\EchoVR\\logs");
  EXPECT_EQ(in.legacy_cosmetics_dir, L"C:\\Users\\u\\AppData\\Local\\EchoVR\\cosmetics\\v1\\packages");
  wchar_t exe[MAX_PATH * 2] = {};
  const DWORD exe_len = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
  ASSERT_NE(exe_len, 0u);
  std::wstring exe_dir(exe, exe_len);
  exe_dir.resize(exe_dir.find_last_of(L"\\/") + 1);
  EXPECT_EQ(in.legacy_plugins_dir, exe_dir + L"plugins") << "the plugins folder next to the game executable";
  ASSERT_EQ(in.local_dirs.size(), 3u);
  EXPECT_NE(in.local_dirs[0].find(L"\\_local"), std::wstring::npos);
  EXPECT_NE(in.local_dirs[1].find(L"..\\_local"), std::wstring::npos);
  EXPECT_NE(in.local_dirs[2].find(L"..\\..\\_local"), std::wstring::npos);
  SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  EXPECT_TRUE(move::InputsFromEnvironment().root.empty());
  if (had != 0) SetEnvironmentVariableW(L"LOCALAPPDATA", saved);
}

}  // namespace

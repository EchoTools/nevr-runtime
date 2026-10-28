// #1255: which config.yaml a run reads. The pure decision (runtime/lifecycle/config_yaml_choice.h) is driven with a fake file
// system; the file-system half (config_yaml_source.cpp) against a real temp tree, with a non-ASCII folder on purpose; the
// rejection (cli_reject.cpp) and the wiring in service_config.cpp through child processes that end the way a run does.
// Built into test_service_config; run under Wine by `just test-auth-unit`.

#include <gtest/gtest.h>

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/logging.h"
#include "core/nevr_root.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/cli_reject.h"
#include "runtime/lifecycle/config_yaml_choice.h"
#include "runtime/lifecycle/config_yaml_source.h"
#include "runtime/lifecycle/service_config.h"

namespace {

namespace cy = nevr_config_yaml;

// --- a fake file system ---------------------------------------------------------------------------------------------------

struct FakeFs {
  std::set<std::string> files;
  std::set<std::string> unreadable;
  cy::Probe Probe() const {
    cy::Probe p;
    p.exists = [this](const std::string& path) { return files.count(path) != 0; };
    p.readable = [this](const std::string& path) { return files.count(path) != 0 && unreadable.count(path) == 0; };
    return p;
  }
};

const char* const kRoot = "C:\\Users\\u\\AppData\\Local\\nevr\\echovr";
const char* const kRootYaml = "C:\\Users\\u\\AppData\\Local\\nevr\\echovr\\config.yaml";
const char* const kModuleDir = "C:\\game\\bin\\win10\\";
const char* const kLegacy0 = "C:\\game\\bin\\win10\\_local\\config.yaml";
const char* const kLegacy1 = "C:\\game\\bin\\win10\\..\\_local\\config.yaml";
const char* const kLegacy2 = "C:\\game\\bin\\win10\\..\\..\\_local\\config.yaml";

cy::Inputs BaseInputs() {
  cy::Inputs in;
  in.root = kRoot;
  in.legacy = cy::LegacyCandidates(kModuleDir);
  return in;
}

// --- the paths ---------------------------------------------------------------------------------------------------------

TEST(ConfigYamlPaths, TheRootsConfigYamlIsTheRootJoinedWithItsName) {
  EXPECT_EQ(nevr_root::ConfigYamlPath(std::string(kRoot)), kRootYaml);
  EXPECT_EQ(nevr_root::ConfigYamlPath(std::wstring(L"C:\\r")), L"C:\\r\\config.yaml");
  EXPECT_EQ(nevr_root::ConfigYamlPath(std::string()), "") << "an unresolved root stays unresolved";
}

TEST(ConfigYamlPaths, TheLegacyCandidatesAreTheThreeLocalFoldersTheOldSearchUsed) {
  const std::vector<std::string> want = {kLegacy0, kLegacy1, kLegacy2};
  EXPECT_EQ(cy::LegacyCandidates(kModuleDir), want);
}

// --- the decision ------------------------------------------------------------------------------------------------------

TEST(ConfigYamlChoose, TheRootsConfigIsReadWhenNothingElseIsNamed) {
  FakeFs fs;
  fs.files = {kRootYaml};
  const cy::Choice c = cy::Choose(BaseInputs(), fs.Probe());
  EXPECT_EQ(c.rejection, cy::Rejection::kNone);
  EXPECT_EQ(c.path, kRootYaml);
  EXPECT_EQ(c.chosen_by, cy::ChosenBy::kNevrRoot);
  EXPECT_STREQ(cy::ChosenByName(c.chosen_by), "nevr root");
  EXPECT_TRUE(c.copy_from.empty());
  EXPECT_TRUE(c.ignored_legacy.empty());
}

TEST(ConfigYamlChoose, ConfigFileWinsOverTheRoot) {
  FakeFs fs;
  fs.files = {kRootYaml, "D:\\mine\\cfg.yaml"};
  cy::Inputs in = BaseInputs();
  in.config_file_flag = true;
  in.config_file = "D:\\mine\\cfg.yaml";
  const cy::Choice c = cy::Choose(in, fs.Probe());
  EXPECT_EQ(c.rejection, cy::Rejection::kNone);
  EXPECT_EQ(c.path, "D:\\mine\\cfg.yaml");
  EXPECT_EQ(c.chosen_by, cy::ChosenBy::kConfigFile);
  EXPECT_STREQ(cy::ChosenByName(c.chosen_by), "-config-file");
}

TEST(ConfigYamlChoose, ConfigFileGivenWithoutAValueIsRejected) {
  FakeFs fs;
  fs.files = {kRootYaml};
  cy::Inputs in = BaseInputs();
  in.config_file_flag = true;
  const cy::Choice c = cy::Choose(in, fs.Probe());
  EXPECT_EQ(c.rejection, cy::Rejection::kConfigFileNoValue);
  EXPECT_TRUE(c.path.empty()) << "no fallback after a rejection";
  const cy::RejectionText text = cy::DescribeRejection(c);
  EXPECT_EQ(text.argument, "-config-file");
  EXPECT_NE(text.why.find("file"), std::string::npos);
}

TEST(ConfigYamlChoose, ConfigFileNamingAMissingFileIsRejectedWithItsPath) {
  FakeFs fs;
  fs.files = {kRootYaml};  // the root has one: still no fallback
  cy::Inputs in = BaseInputs();
  in.config_file_flag = true;
  in.config_file = "D:\\gone\\cfg.yaml";
  const cy::Choice c = cy::Choose(in, fs.Probe());
  EXPECT_EQ(c.rejection, cy::Rejection::kConfigFileMissing);
  EXPECT_TRUE(c.path.empty());
  const cy::RejectionText text = cy::DescribeRejection(c);
  EXPECT_EQ(text.argument, "-config-file");
  EXPECT_NE(text.why.find("D:\\gone\\cfg.yaml"), std::string::npos);
  EXPECT_NE(text.why.find("does not exist"), std::string::npos);
}

TEST(ConfigYamlChoose, ConfigFileNamingAFileThatCannotBeOpenedIsRejectedWithItsPath) {
  FakeFs fs;
  fs.files = {"D:\\locked\\cfg.yaml"};
  fs.unreadable = {"D:\\locked\\cfg.yaml"};
  cy::Inputs in = BaseInputs();
  in.config_file_flag = true;
  in.config_file = "D:\\locked\\cfg.yaml";
  const cy::Choice c = cy::Choose(in, fs.Probe());
  EXPECT_EQ(c.rejection, cy::Rejection::kConfigFileUnreadable);
  const cy::RejectionText text = cy::DescribeRejection(c);
  EXPECT_NE(text.why.find("D:\\locked\\cfg.yaml"), std::string::npos);
  EXPECT_NE(text.why.find("cannot be opened for reading"), std::string::npos);
}

TEST(ConfigYamlChoose, ConfigPathIsRejectedWithTheHintWhateverElseIsGiven) {
  FakeFs fs;
  fs.files = {kRootYaml, "D:\\mine\\cfg.yaml"};
  cy::Inputs in = BaseInputs();
  in.config_path_flag = true;
  in.config_file_flag = true;
  in.config_file = "D:\\mine\\cfg.yaml";
  const cy::Choice c = cy::Choose(in, fs.Probe());
  EXPECT_EQ(c.rejection, cy::Rejection::kConfigPathFlag);
  EXPECT_TRUE(c.path.empty()) << "-config-path is never honoured";
  const cy::RejectionText text = cy::DescribeRejection(c);
  EXPECT_EQ(text.argument, "-config-path");
  EXPECT_EQ(text.why, "use -config-file <file>");
}

TEST(ConfigYamlChoose, WithNoFileAnywhereNothingIsChosenAndTheRootsPathIsTheOnePlaceSearched) {
  FakeFs fs;
  const cy::Choice c = cy::Choose(BaseInputs(), fs.Probe());
  EXPECT_EQ(c.rejection, cy::Rejection::kNone);
  EXPECT_TRUE(c.path.empty());
  EXPECT_EQ(c.chosen_by, cy::ChosenBy::kNone);
  EXPECT_EQ(c.root_path, kRootYaml);
  EXPECT_TRUE(c.copy_from.empty());
}

TEST(ConfigYamlChoose, AnEmptyRootSearchesNothingAndCopiesNothing) {
  FakeFs fs;
  fs.files = {kLegacy0};
  cy::Inputs in = BaseInputs();
  in.root.clear();
  const cy::Choice c = cy::Choose(in, fs.Probe());
  EXPECT_TRUE(c.path.empty());
  EXPECT_TRUE(c.root_path.empty());
  EXPECT_TRUE(c.copy_from.empty()) << "nowhere to copy to";
}

TEST(ConfigYamlChoose, ALegacyFileIsNeverTheFileToRead) {
  FakeFs fs;
  fs.files = {kLegacy0, kLegacy1, kLegacy2};
  const cy::Choice c = cy::Choose(BaseInputs(), fs.Probe());
  EXPECT_TRUE(c.path.empty());
  EXPECT_EQ(c.chosen_by, cy::ChosenBy::kNone);
}

TEST(ConfigYamlChoose, TheFirstLegacyFileByTheOldOrderIsTheOneToCopyIntoAnEmptyRoot) {
  FakeFs fs;
  fs.files = {kLegacy1, kLegacy2};
  const cy::Choice c = cy::Choose(BaseInputs(), fs.Probe());
  EXPECT_EQ(c.copy_from, kLegacy1);
  EXPECT_EQ(c.root_path, kRootYaml);
  EXPECT_TRUE(c.ignored_legacy.empty());
  EXPECT_TRUE(c.path.empty()) << "chosen only once the copy has happened";
}

TEST(ConfigYamlChoose, ALegacyFileIsIgnoredWhenTheRootHasOne) {
  FakeFs fs;
  fs.files = {kRootYaml, kLegacy2, kLegacy0};
  const cy::Choice c = cy::Choose(BaseInputs(), fs.Probe());
  EXPECT_EQ(c.path, kRootYaml);
  EXPECT_TRUE(c.copy_from.empty());
  EXPECT_EQ(c.ignored_legacy, kLegacy0) << "the first one found by the old order";
}

TEST(ConfigYamlChoose, TheCopyIsDueWhateverFileTheCommandLineNames) {
  FakeFs fs;
  fs.files = {kLegacy0, "D:\\mine\\cfg.yaml"};
  cy::Inputs in = BaseInputs();
  in.config_file_flag = true;
  in.config_file = "D:\\mine\\cfg.yaml";
  const cy::Choice c = cy::Choose(in, fs.Probe());
  EXPECT_EQ(c.path, "D:\\mine\\cfg.yaml");
  EXPECT_EQ(c.copy_from, kLegacy0);
}

// --- the stop line -----------------------------------------------------------------------------------------------------

TEST(CliRejectLine, IsOneJsonObjectWithTheFieldsAStopLineCarries) {
  const std::string line = nevr_cli_reject::BuildStopLine("-config-file", "D:\\a\\b.yaml: file does not exist", "5.7.1",
                                                          "2026-10-28T12:00:00.000Z", 4242);
  EXPECT_EQ(line.find('\n'), std::string::npos) << "one line";
  const nlohmann::json j = nlohmann::json::parse(line);
  EXPECT_EQ(j.at("tag"), "NEVR.STOP");
  EXPECT_EQ(j.at("ts"), "2026-10-28T12:00:00.000Z");
  EXPECT_EQ(j.at("pid"), 4242);
  EXPECT_EQ(j.at("event"), "stop");
  EXPECT_EQ(j.at("stopped"), true);
  EXPECT_EQ(j.at("code"), 64);
  EXPECT_EQ(j.at("reason"), "usage");
  EXPECT_EQ(j.at("runtime_version"), "5.7.1");
  EXPECT_EQ(j.at("detail"), "-config-file: D:\\a\\b.yaml: file does not exist")
      << "the quotes and backslashes of a path survive the round trip";
}

TEST(CliRejectLine, TheVersionIsTheSecondWordOfTheIdentityLiteral) {
  EXPECT_EQ(nevr_cli_reject::VersionOfIdentity("NEVR-BUILD 5.0.1-dev.3+abc1234 3a35e0b945456dba8a9b56e8110df330178e7259"),
            "5.0.1-dev.3+abc1234");
  EXPECT_EQ(nevr_cli_reject::VersionOfIdentity("NEVR-BUILD 5.0.0 3a35e0b945456dba8a9b56e8110df330178e7259"), "5.0.0");
  EXPECT_EQ(nevr_cli_reject::VersionOfIdentity("not an identity"), "");
  EXPECT_EQ(nevr_cli_reject::VersionOfIdentity(nullptr), "");
}

TEST(CliRejectExit, TheProcessEndsWithCode64AndOneLineOnStderr) {
  ASSERT_EXIT(RejectCommandLine("-config-path", "use -config-file <file>"), ::testing::ExitedWithCode(64),
              "\"tag\":\"NEVR\\.STOP\".*\"code\":64.*\"reason\":\"usage\".*"
              "\"detail\":\"-config-path: use -config-file <file>\"");
}

// --- the file-system half, against a real tree -------------------------------------------------------------------------

std::wstring W(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string U(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(std::filesystem::path(W(path)), std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void WriteText(const std::string& path, const std::string& text) {
  std::filesystem::create_directories(std::filesystem::path(W(path)).parent_path());
  std::ofstream out(std::filesystem::path(W(path)), std::ios::binary);
  out << text;
  ASSERT_TRUE(out.good());
}

bool Exists(const std::string& path) { return GetFileAttributesW(W(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

FILETIME LastWrite(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &data);
  return data.ftLastWriteTime;
}

// Every file and folder under `dir`, as text, sorted.
std::vector<std::string> Listing(const std::string& dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(std::filesystem::path(W(dir)), ec)) {
    names.push_back(U(entry.path().wstring()));
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string>& Captured() {
  static std::vector<std::string> lines;
  return lines;
}

VOID CaptureSink(EchoVR::LogLevel level, const CHAR* message) {
  Captured().push_back(std::string(level == EchoVR::LogLevel::Warning ? "W|"
                                   : level == EchoVR::LogLevel::Info  ? "I|"
                                   : level == EchoVR::LogLevel::Error ? "E|"
                                                                      : "?|") +
                       message);
}

std::string Joined() {
  std::string all;
  for (const std::string& line : Captured()) all += line + "\n";
  return all;
}

// <base>\game\bin\win10  (the module folder), with `_local` below it and one and two folders up;
// <base>\appdata         (the Local AppData folder), whose nevr\echovr is the root.
class ConfigYamlFiles : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    base_ = U(std::wstring(tmp)) + "nevr-1255-" + std::to_string(GetCurrentProcessId()) + "-\xC3\xA9\xE4\xB8\xAD-" + test;
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(W(base_)), ec);
    module_dir_ = base_ + "\\game\\bin\\win10\\";
    root_ = nevr_root::RootUnder(base_ + "\\appdata");
    root_yaml_ = nevr_root::ConfigYamlPath(root_);
    legacy_ = cy::LegacyCandidates(module_dir_);
    ASSERT_EQ(legacy_.size(), 3u) << "the three places the old search used";
    Captured().clear();
    SetBootLogSink(&CaptureSink);
  }
  void TearDown() override {
    SetBootLogSink(nullptr);
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(W(base_)), ec);
  }
  cy::Inputs Inputs() const {
    cy::Inputs in;
    in.root = root_;
    in.legacy = legacy_;
    return in;
  }
  std::string Game() const { return base_ + "\\game"; }

  std::string base_, module_dir_, root_, root_yaml_;
  std::vector<std::string> legacy_;
};

TEST_F(ConfigYamlFiles, ALegacyFileIsCopiedOnceIntoAnEmptyRootAndTheOriginalIsLeftAsItIs) {
  const std::string text = "version: \"1\"\r\nservices:\r\n  socket_uri: \"ws://legacy.example:80/spr\"\r\n";
  WriteText(legacy_[1], text);
  const FILETIME before = LastWrite(legacy_[1]);
  const std::vector<std::string> gameBefore = Listing(Game());

  const cy::Choice c = cy::Resolve(Inputs(), cy::RealProbe());

  EXPECT_EQ(c.path, root_yaml_) << "the copy in the root is what is read";
  EXPECT_EQ(c.chosen_by, cy::ChosenBy::kNevrRoot);
  EXPECT_EQ(ReadAll(root_yaml_), text) << "the copy equals the original";
  EXPECT_EQ(ReadAll(legacy_[1]), text) << "the original is byte for byte what it was";
  const FILETIME after = LastWrite(legacy_[1]);
  EXPECT_EQ(before.dwLowDateTime, after.dwLowDateTime);
  EXPECT_EQ(before.dwHighDateTime, after.dwHighDateTime);
  EXPECT_EQ(Listing(Game()), gameBefore) << "nothing is created, renamed or deleted in the game folder";

  const std::string log = Joined();
  EXPECT_NE(log.find("I|[NEVR.CONFIG] config.yaml copied from " + legacy_[1] + " to " + root_yaml_ +
                     "; the old file was left as it is, delete it"),
            std::string::npos)
      << log;
  EXPECT_EQ(std::count(log.begin(), log.end(), '\n'), 1) << "one line: " << log;
}

TEST_F(ConfigYamlFiles, ASecondRunFindsTheRootsFileAndCopiesNothing) {
  WriteText(legacy_[0], "a: 1\n");
  (void)cy::Resolve(Inputs(), cy::RealProbe());
  Captured().clear();
  WriteText(root_yaml_, "edited: in the root\n");

  const cy::Choice c = cy::Resolve(Inputs(), cy::RealProbe());

  EXPECT_EQ(c.path, root_yaml_);
  EXPECT_EQ(ReadAll(root_yaml_), "edited: in the root\n") << "a root file is never overwritten";
  const std::string log = Joined();
  EXPECT_EQ(log.find("copied"), std::string::npos) << log;
}

TEST_F(ConfigYamlFiles, ALegacyFileIsIgnoredWithOneWarningWhenTheRootHasOneAndNeitherFileIsTouched) {
  WriteText(root_yaml_, "root: yes\n");
  WriteText(legacy_[2], "legacy: yes\n");
  const FILETIME rootBefore = LastWrite(root_yaml_);
  const FILETIME legacyBefore = LastWrite(legacy_[2]);
  const std::vector<std::string> gameBefore = Listing(Game());

  const cy::Choice c = cy::Resolve(Inputs(), cy::RealProbe());

  EXPECT_EQ(c.path, root_yaml_);
  EXPECT_EQ(ReadAll(root_yaml_), "root: yes\n");
  EXPECT_EQ(ReadAll(legacy_[2]), "legacy: yes\n");
  EXPECT_EQ(LastWrite(root_yaml_).dwLowDateTime, rootBefore.dwLowDateTime);
  EXPECT_EQ(LastWrite(legacy_[2]).dwLowDateTime, legacyBefore.dwLowDateTime);
  EXPECT_EQ(Listing(Game()), gameBefore);
  const std::string log = Joined();
  EXPECT_NE(log.find("W|[NEVR.CONFIG] " + legacy_[2] + " is ignored"), std::string::npos) << log;
  EXPECT_NE(log.find(root_yaml_), std::string::npos) << "the warning names the root's file too: " << log;
  EXPECT_EQ(std::count(log.begin(), log.end(), '\n'), 1) << log;
}

TEST_F(ConfigYamlFiles, ACopyThatFailsIsAWarningWithTheWin32ErrorAndTheRunHasNoFileFromTheRoot) {
  WriteText(legacy_[0], "a: 1\n");
  // The root's own folder is a file, so the root cannot be created.
  WriteText(base_ + "\\appdata\\nevr", "in the way");

  const cy::Choice c = cy::Resolve(Inputs(), cy::RealProbe());

  EXPECT_TRUE(c.path.empty());
  EXPECT_EQ(ReadAll(legacy_[0]), "a: 1\n");
  const std::string log = Joined();
  EXPECT_NE(log.find("W|[NEVR.CONFIG] could not copy config.yaml from " + legacy_[0] + " to " + root_yaml_ + " (error "),
            std::string::npos)
      << log;
}

TEST_F(ConfigYamlFiles, NothingAnywhereCreatesNothing) {
  WriteText(base_ + "\\game\\bin\\win10\\echovr.exe", "x");
  const std::vector<std::string> before = Listing(base_);
  const cy::Choice c = cy::Resolve(Inputs(), cy::RealProbe());
  EXPECT_TRUE(c.path.empty());
  EXPECT_EQ(c.root_path, root_yaml_);
  EXPECT_EQ(Listing(base_), before);
  EXPECT_EQ(Joined(), "") << "the caller logs the no-file Warning";
}

TEST_F(ConfigYamlFiles, ConfigFileNamingAMissingFileOrAFolderIsRejectedByTheRealProbe) {
  cy::Inputs in = Inputs();
  in.config_file_flag = true;
  in.config_file = base_ + "\\absent.yaml";
  EXPECT_EQ(cy::Resolve(in, cy::RealProbe()).rejection, cy::Rejection::kConfigFileMissing);

  in.config_file = base_ + "\\game";  // exists, cannot be read as a file
  ASSERT_TRUE(Exists(in.config_file));
  EXPECT_EQ(cy::Resolve(in, cy::RealProbe()).rejection, cy::Rejection::kConfigFileUnreadable);

  WriteText(base_ + "\\here.yaml", "k: v\n");
  in.config_file = base_ + "\\here.yaml";
  const cy::Choice ok = cy::Resolve(in, cy::RealProbe());
  EXPECT_EQ(ok.rejection, cy::Rejection::kNone);
  EXPECT_EQ(ok.path, in.config_file);
}

TEST_F(ConfigYamlFiles, ReadWholeFileReadsANonAsciiPathAndReportsTheErrorForAMissingOne) {
  WriteText(base_ + "\\f.yaml", "k: v\n");
  std::string bytes;
  unsigned long error = 0;
  EXPECT_TRUE(cy::ReadWholeFile(base_ + "\\f.yaml", &bytes, &error));
  EXPECT_EQ(bytes, "k: v\n");
  EXPECT_FALSE(cy::ReadWholeFile(base_ + "\\none.yaml", &bytes, &error));
  EXPECT_EQ(error, static_cast<unsigned long>(ERROR_FILE_NOT_FOUND));
}

TEST_F(ConfigYamlFiles, CopyIfAbsentNeverReplacesAFile) {
  WriteText(base_ + "\\from.yaml", "from\n");
  WriteText(base_ + "\\to.yaml", "already here\n");
  unsigned long error = 0;
  EXPECT_FALSE(cy::CopyIfAbsent(base_ + "\\from.yaml", base_ + "\\to.yaml", &error));
  EXPECT_EQ(error, static_cast<unsigned long>(ERROR_FILE_EXISTS));
  EXPECT_EQ(ReadAll(base_ + "\\to.yaml"), "already here\n");
  EXPECT_TRUE(cy::CopyIfAbsent(base_ + "\\from.yaml", base_ + "\\deeper\\new\\to.yaml", &error));
  EXPECT_EQ(ReadAll(base_ + "\\deeper\\new\\to.yaml"), "from\n");
}

TEST(ConfigYamlProcessPaths, TheRootAndTheLegacyCandidatesComeFromTheEnvironmentAndTheModuleFolder) {
  ASSERT_TRUE(SetEnvironmentVariableA("LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local"));
  cy::Inputs in;
  cy::AddProcessPaths(kModuleDir, &in);
  EXPECT_EQ(in.root, kRoot);
  EXPECT_EQ(in.legacy, cy::LegacyCandidates(kModuleDir));
  ASSERT_TRUE(SetEnvironmentVariableA("LOCALAPPDATA", nullptr));
  cy::Inputs none;
  cy::AddProcessPaths(kModuleDir, &none);
  EXPECT_TRUE(none.root.empty());
}

// --- the wiring in service_config.cpp: each case is a child process that ends the way a run does -----------------------------

// A throwaway LOCALAPPDATA with a root config.yaml, fixed by test name so the child finds what the parent wrote.
std::string AppDataFor(const char* name, const char* yaml) {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::string appdata = U(std::wstring(tmp)) + "nevr-1255-wiring-" + name;
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(W(appdata)), ec);
  if (yaml != nullptr) WriteText(nevr_root::ConfigYamlPath(nevr_root::RootUnder(appdata)), yaml);
  else std::filesystem::create_directories(std::filesystem::path(W(appdata)));
  return appdata;
}

constexpr const char* kYaml = "version: \"1\"\nservices:\n  socket_uri: \"ws://from-file.example:80/spr\"\n";

// Reads one key the way the runtime does, which loads config.yaml the first time, then ends the process.
[[noreturn]] void LoadAndExit(const std::string& appdata) {
  SetEnvironmentVariableA("LOCALAPPDATA", appdata.c_str());
  NevrCfgResolveConfigYaml();
  const char* socket = NevrCfgGetFlat("nevr_socket_uri");
  ExitProcess(socket != nullptr && std::string(socket) == "ws://from-file.example:80/spr" ? 0 : 7);
}

TEST(ConfigYamlWiring, ARootConfigYamlIsLoadedAndTheLineNamesTheFileAndTheNevrRoot) {
  const std::string appdata = AppDataFor("root", kYaml);
  ASSERT_EXIT(LoadAndExit(appdata), ::testing::ExitedWithCode(0),
              "\\[NEVR\\.CONFIG\\] config\\.yaml loaded from: .*nevr.echovr.config\\.yaml chosen_by=nevr root");
}

TEST(ConfigYamlWiring, ConfigFileWinsOverTheRootAndTheLineSaysSo) {
  const std::string appdata = AppDataFor("flag", "version: \"1\"\nservices:\n  socket_uri: \"ws://root.example:80/spr\"\n");
  const std::string mine = appdata + "\\elsewhere\\mine.yaml";
  WriteText(mine, kYaml);
  ASSERT_EXIT(
      {
        g_configYamlFlags.config_file_given = true;
        g_configYamlFlags.config_file = mine;
        SetEnvironmentVariableA("LOCALAPPDATA", appdata.c_str());
        NevrCfgResolveConfigYaml();
        const char* socket = NevrCfgGetFlat("nevr_socket_uri");
        ExitProcess(socket != nullptr && std::string(socket) == "ws://from-file.example:80/spr" ? 0 : 7);
      },
      ::testing::ExitedWithCode(0), "config\\.yaml loaded from: .*mine\\.yaml chosen_by=-config-file");
}

TEST(ConfigYamlWiring, NoFileAnywhereIsOneWarningNamingTheOnePlaceSearched) {
  const std::string appdata = AppDataFor("none", nullptr);
  ASSERT_EXIT(
      {
        SetEnvironmentVariableA("LOCALAPPDATA", appdata.c_str());
        NevrCfgResolveConfigYaml();
        (void)NevrCfgGetFlat("nevr_socket_uri");
        ExitProcess(0);
      },
      ::testing::ExitedWithCode(0), "\\[NEVR\\.CONFIG\\] no config\\.yaml found; searched: .*nevr.echovr.config\\.yaml");
}

TEST(ConfigYamlWiring, AMissingConfigFileEndsTheRunWithCode64AndTheLineBeforeAnyConfigIsRead) {
  const std::string appdata = AppDataFor("missing", kYaml);
  ASSERT_EXIT(
      {
        g_configYamlFlags.config_file_given = true;
        g_configYamlFlags.config_file = appdata + "\\nope.yaml";
        SetEnvironmentVariableA("LOCALAPPDATA", appdata.c_str());
        NevrCfgResolveConfigYaml();
        ExitProcess(0);
      },
      ::testing::ExitedWithCode(64), "\"code\":64.*\"detail\":\"-config-file: .*nope\\.yaml: file does not exist\"");
}

TEST(ConfigYamlWiring, ConfigFileWithoutAValueEndsTheRunWithCode64) {
  const std::string appdata = AppDataFor("novalue", kYaml);
  ASSERT_EXIT(
      {
        g_configYamlFlags.config_file_given = true;
        SetEnvironmentVariableA("LOCALAPPDATA", appdata.c_str());
        NevrCfgResolveConfigYaml();
        ExitProcess(0);
      },
      ::testing::ExitedWithCode(64), "\"detail\":\"-config-file: ");
}

TEST(ConfigYamlWiring, ConfigPathEndsTheRunWithCode64AndTheHint) {
  const std::string appdata = AppDataFor("path", kYaml);
  ASSERT_EXIT(
      {
        g_configYamlFlags.config_path_given = true;
        SetEnvironmentVariableA("LOCALAPPDATA", appdata.c_str());
        NevrCfgResolveConfigYaml();
        ExitProcess(0);
      },
      ::testing::ExitedWithCode(64), "\"detail\":\"-config-path: use -config-file <file>\"");
}

}  // namespace

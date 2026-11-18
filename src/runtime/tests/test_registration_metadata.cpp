// The per-install fields of a GameServerRegistration (runtime/server/registration_metadata.cpp): the levels the install's
// package manifests list, the regions and guilds config.yaml names, and the features the combat module states. The code
// is the production code; config.yaml, the combat module and the plugin assets are stood in for below, and the install is
// a folder this test lays out.
//
// NOT shown here: a registration against a real install's manifests (the opt-in test in test_installed_levels.cpp reads
// one), and FillRegistrationMetadata's own choice of the running exe (GetModuleFileNameW), which is one line.

#include <gtest/gtest.h>
#include <zstd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "runtime/combat/combat.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/server/installed_levels.h"
#include "runtime/server/registered_levels.h"
#include "runtime/server/registration_metadata.h"
#include "runtime/tests/run_root.h"

// ---- what the code under test reads, stood in for ----

namespace {
std::map<std::string, std::string> g_flatCsv;  // config.yaml's flat keys
std::vector<std::string> g_features;           // what the combat module started
bool g_featuresThrow = false;
std::vector<std::pair<std::uint64_t, std::uint64_t>> g_registered;  // plugin and resource-set level registrations
}  // namespace

const char* NevrCfgGetFlatCsv(const char* flatKey) {
  const auto it = g_flatCsv.find(flatKey);
  return it == g_flatCsv.end() ? nullptr : it->second.c_str();
}

namespace nevr_combat {
std::vector<std::string> ServerFeatures() {
  if (g_featuresThrow) throw std::runtime_error("features unavailable");
  return g_features;
}
}  // namespace nevr_combat

namespace nevr_game_server {
std::vector<std::pair<std::uint64_t, std::uint64_t>> RegisteredLevelKeys() { return g_registered; }
}  // namespace nevr_game_server

namespace {

using nevr_game_server::BootPackageSets;
using nevr_game_server::FillRegistrationMetadata;
using nevr_game_server::FillRegistrationMetadataForExe;
using nevr_game_server::kLevelRootType;
using nevr_game_server::RegistrationParams;

std::vector<std::pair<EchoVR::LogLevel, std::string>> g_logged;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) { g_logged.emplace_back(level, message); }

const std::pair<EchoVR::LogLevel, std::string>* Find(const std::string& needle) {
  for (const auto& line : g_logged) {
    if (line.second.find(needle) != std::string::npos) return &line;
  }
  return nullptr;
}

constexpr std::uint64_t kLevelA = 0x00000000000000A1ULL;
constexpr std::uint64_t kLevelB = 0x00000000000000B2ULL;
constexpr std::uint64_t kOtherType = 0x1111111111111111ULL;
constexpr std::uint64_t kUnrelatedRegisteredValue = 0x99;
constexpr std::uint64_t kCallerSuppliedLevel = 7;
constexpr std::size_t kBootSetCount = 2;

constexpr std::size_t kU32Bytes = 4;
constexpr std::size_t kU64Bytes = 8;
constexpr std::size_t kBitsPerByte = 8;
// Manifest body layout: a header, then one entry per level root.
constexpr std::size_t kBodyHeaderSize = 0xC0;
constexpr std::size_t kLevelEntrySize = 0x20;
constexpr std::size_t kBodyLevelCountOffset = 0x38;
constexpr std::size_t kEntryLevelIdOffset = 8;
constexpr int kZstdCompressionLevel = 3;
// Manifest container: magic, flags, body size, frame size.
constexpr std::size_t kContainerSize = 24;
constexpr std::string_view kContainerMagic = "ZSTD";
constexpr std::size_t kContainerFlagsOffset = 4;
constexpr std::uint32_t kContainerFlags = 0x10;
constexpr std::size_t kContainerBodySizeOffset = 8;
constexpr std::size_t kContainerFrameSizeOffset = 16;
constexpr std::size_t kSetFileNameDigits = 16;

void PutU32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  for (std::size_t i = 0; i < kU32Bytes; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (kBitsPerByte * i));
}
void PutU64(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value) {
  for (std::size_t i = 0; i < kU64Bytes; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (kBitsPerByte * i));
}

// A manifest file image listing these level roots: a 24-byte container, then one zstd frame of the body
// (docs/reference/package-manifest-format.md; the layout test_installed_levels.cpp builds).
std::vector<std::uint8_t> MakeManifest(const std::vector<std::uint64_t>& levels) {
  const std::size_t bodySize = kBodyHeaderSize + kLevelEntrySize * levels.size();
  std::vector<std::uint8_t> body(bodySize, 0);
  PutU32(body, 0x00, 1);
  PutU64(body, kBodyLevelCountOffset, levels.size());
  for (std::size_t i = 0; i < levels.size(); ++i) {
    PutU64(body, kBodyHeaderSize + kLevelEntrySize * i, kLevelRootType);
    PutU64(body, kBodyHeaderSize + kLevelEntrySize * i + kEntryLevelIdOffset, levels[i]);
  }
  std::vector<std::uint8_t> frame(ZSTD_compressBound(body.size()));
  frame.resize(ZSTD_compress(frame.data(), frame.size(), body.data(), body.size(), kZstdCompressionLevel));
  std::vector<std::uint8_t> file(kContainerSize, 0);
  for (std::size_t i = 0; i < kContainerMagic.size(); ++i) file[i] = static_cast<std::uint8_t>(kContainerMagic[i]);
  PutU32(file, kContainerFlagsOffset, kContainerFlags);
  PutU64(file, kContainerBodySizeOffset, body.size());
  PutU64(file, kContainerFrameSizeOffset, frame.size());
  file.insert(file.end(), frame.begin(), frame.end());
  return file;
}

std::string SetFileName(std::uint64_t setId) {
  char buf[kSetFileNameDigits + 1];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(setId));
  return buf;
}

class RegistrationMetadata : public ::testing::Test {
 protected:
  void SetUp() override {
    g_flatCsv.clear();
    g_features.clear();
    g_featuresThrow = false;
    g_registered.clear();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    // An install laid out as the engine does: <root>\bin\win10\echovr.exe, <root>\_data\<version>\rad15\win10\manifests.
    root_ = nevr_test::NewDir(L"registration-metadata");
    ASSERT_FALSE(root_.empty());
    std::filesystem::create_directories(root_ / "bin" / "win10");
    exe_ = root_ / "bin" / "win10" / "echovr.exe";
  }
  void TearDown() override { SetBootLogSink(nullptr); }

  std::filesystem::path ManifestsDir() {
    const std::filesystem::path dir = root_ / "_data" / "v1" / "rad15" / "win10" / "manifests";
    std::filesystem::create_directories(dir);
    return dir;
  }
  void WriteManifest(std::uint64_t setId, const std::vector<std::uint64_t>& levels) {
    const std::vector<std::uint8_t> bytes = MakeManifest(levels);
    std::ofstream out(ManifestsDir() / SetFileName(setId), std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  std::filesystem::path root_;
  std::filesystem::path exe_;
};

TEST_F(RegistrationMetadata, TheLevelsOfEveryBootSetsManifestAreReportedSortedWithoutDuplicates) {
  const std::vector<std::uint64_t> sets = BootPackageSets();
  ASSERT_EQ(sets.size(), kBootSetCount);
  WriteManifest(sets[0], {kLevelB, kLevelA});
  WriteManifest(sets[1], {kLevelA});
  RegistrationParams params;
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_EQ(params.installedLevels, (std::vector<std::uint64_t>{kLevelA, kLevelB}));
  const auto* summary = Find("registration metadata levels=2 manifests_read=2 plugin_levels=0");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->first, EchoVR::LogLevel::Info);
  EXPECT_EQ(Find("level manifest reader failed"), nullptr);
}

TEST_F(RegistrationMetadata, AMissingSetIsAProblemLineAndTheOtherSetStillCounts) {
  WriteManifest(BootPackageSets()[0], {kLevelA});
  RegistrationParams params;
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_EQ(params.installedLevels, (std::vector<std::uint64_t>{kLevelA}));
  const auto* problem = Find("level manifest reader failed: " + ManifestsDir().string() + " " + SetFileName(BootPackageSets()[1]) + ": not found");
  ASSERT_NE(problem, nullptr);
  EXPECT_EQ(problem->first, EchoVR::LogLevel::Warning);
}

TEST_F(RegistrationMetadata, AnInstallWithNoDataFolderReportsNoLevelsAndSaysWhereItLooked) {
  RegistrationParams params;
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_TRUE(params.installedLevels.empty());
  const auto* where = Find("level manifests: no data dir found under " + (root_ / "_data").string());
  ASSERT_NE(where, nullptr);
  EXPECT_EQ(where->first, EchoVR::LogLevel::Warning);
  EXPECT_NE(Find("level manifests: 0 levels from 0 manifest(s) read"), nullptr);
}

TEST_F(RegistrationMetadata, NoExePathReportsNoLevelsButStillFillsTheOtherFields) {
  g_flatCsv["nevr_regions"] = "us-east";
  g_features = {"echocombat"};
  RegistrationParams params;
  FillRegistrationMetadataForExe({}, params);
  EXPECT_TRUE(params.installedLevels.empty());
  EXPECT_NE(Find("level manifests: the game's exe path is unavailable"), nullptr);
  // No exe is not "no data dir": the second warning is only for an exe whose data folder is missing.
  EXPECT_EQ(Find("no data dir found"), nullptr);
  EXPECT_EQ(params.regions, (std::vector<std::string>{"us-east"}));
  EXPECT_EQ(params.features, (std::vector<std::string>{"echocombat"}));
}

TEST_F(RegistrationMetadata, PluginRegisteredLevelsAreMergedInAndCounted) {
  WriteManifest(BootPackageSets()[0], {kLevelA});
  g_registered = {{kLevelRootType, kLevelB}, {kOtherType, kUnrelatedRegisteredValue}};  // only a level-root registration is a level
  RegistrationParams params;
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_EQ(params.installedLevels, (std::vector<std::uint64_t>{kLevelA, kLevelB}));
  EXPECT_NE(Find("registration metadata levels=2 manifests_read=1 plugin_levels=1"), nullptr);
}

TEST_F(RegistrationMetadata, RegionsAndGuildsAreTheConfigListsParsedTheWayServerDbParsesThem) {
  g_flatCsv["nevr_regions"] = "us-west,,eu";
  g_flatCsv["nevr_guilds"] = "123,any";
  RegistrationParams params;
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_EQ(params.regions, (std::vector<std::string>{"eu", "us-west"}));
  EXPECT_EQ(params.guilds, (std::vector<std::string>{"123", "any"}));
  EXPECT_EQ(Find("registration list items rejected"), nullptr);
  EXPECT_NE(Find("regions=2 guilds=2"), nullptr);
}

TEST_F(RegistrationMetadata, ItemsTheServerDbPatternRejectsAreDroppedAndCountedInOneWarning) {
  g_flatCsv["nevr_regions"] = "us west,eu";  // a space is not a region character
  g_flatCsv["nevr_guilds"] = "12,abc,any,x y";
  RegistrationParams params;
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_EQ(params.regions, (std::vector<std::string>{"eu"}));
  EXPECT_EQ(params.guilds, (std::vector<std::string>{"12", "any"}));
  const auto* rejected = Find("registration list items rejected by the ServerDB pattern regions=1 guilds=2");
  ASSERT_NE(rejected, nullptr);
  EXPECT_EQ(rejected->first, EchoVR::LogLevel::Warning);
}

TEST_F(RegistrationMetadata, TheRejectedItemWarningIsWrittenWhenEitherListHadARejection) {
  g_flatCsv["nevr_regions"] = "bad region";
  RegistrationParams onlyRegions;
  FillRegistrationMetadataForExe(exe_, onlyRegions);
  EXPECT_NE(Find("regions=1 guilds=0"), nullptr);

  g_logged.clear();
  g_flatCsv.clear();
  g_flatCsv["nevr_guilds"] = "not-a-guild";
  RegistrationParams onlyGuilds;
  FillRegistrationMetadataForExe(exe_, onlyGuilds);
  EXPECT_NE(Find("regions=0 guilds=1"), nullptr);
}

// FillRegistrationMetadata reads the exe of the running process (here the test binary), so it does not take the no-exe
// path.
TEST_F(RegistrationMetadata, ThePublicEntryPointReadsTheRunningProcessesExe) {
  RegistrationParams params;
  FillRegistrationMetadata(params);
  EXPECT_EQ(Find("the game's exe path is unavailable"), nullptr);
  // Whatever the process's folder holds, the run ends with its one summary line.
  EXPECT_NE(Find("registration metadata levels="), nullptr);
}

TEST_F(RegistrationMetadata, KeysTheConfigDoesNotSetLeaveRegionsAndGuildsEmpty) {
  RegistrationParams params;
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_TRUE(params.regions.empty());
  EXPECT_TRUE(params.guilds.empty());
  EXPECT_TRUE(params.features.empty());
  EXPECT_NE(Find("features=none"), nullptr);
}

TEST_F(RegistrationMetadata, FeaturesAreWhatTheCombatModuleStates) {
  g_features = {"echocombat", "echocombat_medigun"};
  RegistrationParams params;
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_EQ(params.features, (std::vector<std::string>{"echocombat", "echocombat_medigun"}));
  EXPECT_NE(Find("features=echocombat,echocombat_medigun"), nullptr);
}

TEST_F(RegistrationMetadata, AFailureLeavesTheCallersFieldsAsTheyWereAndNamesTheCause) {
  WriteManifest(BootPackageSets()[0], {kLevelA});
  g_flatCsv["nevr_regions"] = "us-east";
  g_featuresThrow = true;  // the last step to run throws, after the levels and lists were computed
  RegistrationParams params;
  params.installedLevels = {kCallerSuppliedLevel};
  params.regions = {"kept"};
  params.guilds = {"1"};
  params.features = {"kept_feature"};
  FillRegistrationMetadataForExe(exe_, params);
  EXPECT_EQ(params.installedLevels, (std::vector<std::uint64_t>{kCallerSuppliedLevel}));
  EXPECT_EQ(params.regions, (std::vector<std::string>{"kept"}));
  EXPECT_EQ(params.guilds, (std::vector<std::string>{"1"}));
  EXPECT_EQ(params.features, (std::vector<std::string>{"kept_feature"}));
  const auto* failed = Find("registration metadata failed (features unavailable)");
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->first, EchoVR::LogLevel::Error);
  EXPECT_EQ(Find("registration metadata levels="), nullptr);
}

}  // namespace

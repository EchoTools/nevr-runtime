// #990: the levels a game server registers as installed_levels, read from the package manifests the engine
// loads at boot (runtime/server/installed_levels.h). Each fixture is a manifest image built here the way the
// engine lays it out (docs/reference/package-manifest-format.md), so no game file is needed.
//
// The one test that reads a real install is gated on NEVR_STOCK_MANIFEST_DIR (the manifests folder of a copy
// of the install). It skips when the variable is unset; a skip is not evidence for the stock count.

#include <gtest/gtest.h>
#include <zstd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "abi/symbol_hash.h"
#include "gameservice/v1/gameservice.pb.h"
#include "runtime/patch/asset_registry.h"
#include "runtime/server/installed_levels.h"
#include "runtime/server/registration_envelope.h"

namespace {

using nevr_game_server::BootPackageSets;
using nevr_game_server::CollectInstalledLevels;
using nevr_game_server::CollectInstalledLevelsFromDirs;
using nevr_game_server::InstalledLevels;
using nevr_game_server::kLevelRootType;
using nevr_game_server::ParseLevelRoots;

struct Record {
  uint64_t type;
  uint64_t name;
};

void PutU32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

void PutU64(std::vector<uint8_t>& bytes, std::size_t at, uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

// The manifest file image for these resource records: a 24-byte container, then one zstd frame of the body.
// resourceCountField overrides the count at body+0x38 (the honest value is records.size()).
std::vector<uint8_t> MakeManifest(const std::vector<Record>& records, uint64_t resourceCountField = UINT64_MAX) {
  const std::size_t bodySize = 0xC0 + 0x20 * records.size();
  std::vector<uint8_t> body(bodySize, 0);
  PutU32(body, 0x00, 1);  // one package
  PutU64(body, 0x38, resourceCountField == UINT64_MAX ? records.size() : resourceCountField);
  for (std::size_t i = 0; i < records.size(); ++i) {
    PutU64(body, 0xC0 + 0x20 * i, records[i].type);
    PutU64(body, 0xC0 + 0x20 * i + 8, records[i].name);
  }
  std::vector<uint8_t> frame(ZSTD_compressBound(body.size()));
  const std::size_t frameBytes = ZSTD_compress(frame.data(), frame.size(), body.data(), body.size(), 3);
  frame.resize(frameBytes);

  std::vector<uint8_t> file(24, 0);
  file[0] = 'Z';
  file[1] = 'S';
  file[2] = 'T';
  file[3] = 'D';
  PutU32(file, 4, 0x10);
  PutU64(file, 8, body.size());
  PutU64(file, 16, frame.size());
  file.insert(file.end(), frame.begin(), frame.end());
  return file;
}

class ScratchDir {
 public:
  ScratchDir() {
    std::mt19937_64 gen(std::random_device{}());
    path_ = std::filesystem::temp_directory_path() / ("installed-levels-" + std::to_string(gen()));
    std::filesystem::create_directories(path_);
  }
  ~ScratchDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

constexpr uint64_t kMplArenaA = 0x576ed3f8428ebc4bULL;  // CSymbol64("mpl_arena_a"), asset-system.md "Names"
constexpr uint64_t kSetNetClient = 0x48037dc70b0ecab2ULL;
constexpr uint64_t kSetNetClientLocale = 0x2b47aab238f60515ULL;

}  // namespace

TEST(InstalledLevels, KeepsOnlyLevelRootRecordsInRecordOrder) {
  const std::vector<uint8_t> file = MakeManifest({
      {kLevelRootType, kMplArenaA},
      {0x1111111111111111ULL, 0x2222222222222222ULL},  // some other resource type
      {kLevelRootType, 0x00000000000000AAULL},
      {0x3333333333333333ULL, 0x4444444444444444ULL},
      {kLevelRootType, 0x00000000000000BBULL},
  });
  std::vector<uint64_t> levels;
  std::string error;
  ASSERT_TRUE(ParseLevelRoots(file, &levels, &error)) << error;
  EXPECT_EQ(levels, (std::vector<uint64_t>{kMplArenaA, 0xAA, 0xBB}));
}

TEST(InstalledLevels, AManifestWithNoResourcesHasNoLevels) {
  std::vector<uint64_t> levels{42};
  std::string error;
  ASSERT_TRUE(ParseLevelRoots(MakeManifest({}), &levels, &error)) << error;
  EXPECT_TRUE(levels.empty());
}

TEST(InstalledLevels, RejectsBytesThatAreNotAManifest) {
  const std::vector<uint8_t> file = {'n', 'o', 't', ' ', 'a', ' ', 'm', 'a', 'n', 'i', 'f', 'e', 's', 't', '.',
                                     '.', '.', '.', '.', '.', '.', '.', '.', '.', '.'};
  std::vector<uint64_t> levels;
  std::string error;
  EXPECT_FALSE(ParseLevelRoots(file, &levels, &error));
  EXPECT_EQ(error, "not a ZSTD manifest");
}

TEST(InstalledLevels, RejectsAnUnknownContainerVersion) {
  std::vector<uint8_t> file = MakeManifest({{kLevelRootType, kMplArenaA}});
  PutU32(file, 4, 0x11);
  std::vector<uint64_t> levels;
  std::string error;
  EXPECT_FALSE(ParseLevelRoots(file, &levels, &error));
  EXPECT_EQ(error, "unsupported container version");
}

TEST(InstalledLevels, RejectsACompressedLengthPastTheFile) {
  std::vector<uint8_t> file = MakeManifest({{kLevelRootType, kMplArenaA}});
  PutU64(file, 16, file.size());  // claims more compressed bytes than the file holds
  std::vector<uint64_t> levels;
  std::string error;
  EXPECT_FALSE(ParseLevelRoots(file, &levels, &error));
  EXPECT_EQ(error, "compressed length runs past the end of the file");
}

TEST(InstalledLevels, RejectsAFrameThatDoesNotDecodeToTheDeclaredLength) {
  std::vector<uint8_t> file = MakeManifest({{kLevelRootType, kMplArenaA}});
  PutU64(file, 8, 0xC0 + 0x20 + 1);  // one byte more than the frame decodes to
  std::vector<uint64_t> levels;
  std::string error;
  EXPECT_FALSE(ParseLevelRoots(file, &levels, &error));
  EXPECT_EQ(error, "frame declares a different size than the container");
}

TEST(InstalledLevels, RejectsAResourceCountThatRunsPastTheBody) {
  const std::vector<uint8_t> file = MakeManifest({{kLevelRootType, kMplArenaA}}, /*resourceCountField=*/1000);
  std::vector<uint64_t> levels;
  std::string error;
  EXPECT_FALSE(ParseLevelRoots(file, &levels, &error));
  EXPECT_EQ(error, "resource count runs past the decompressed body");
}

TEST(InstalledLevels, CollectsEverySetThatIsPresentAndReportsTheMissingOne) {
  ScratchDir dir;
  WriteFile(dir.Path() / "48037dc70b0ecab2",
            MakeManifest({{kLevelRootType, kMplArenaA}, {kLevelRootType, 0x00000000000000AAULL}}));
  const InstalledLevels result = CollectInstalledLevels(dir.Path(), {kSetNetClient, kSetNetClientLocale});
  EXPECT_EQ(result.levels, (std::vector<uint64_t>{0xAA, kMplArenaA}));  // sorted
  EXPECT_EQ(result.manifestsRead, 1u);
  ASSERT_EQ(result.problems.size(), 1u);
  EXPECT_EQ(result.problems[0], "2b47aab238f60515: not found");
}

TEST(InstalledLevels, ALevelInTwoSetsIsReportedOnce) {
  ScratchDir dir;
  WriteFile(dir.Path() / "48037dc70b0ecab2", MakeManifest({{kLevelRootType, kMplArenaA}}));
  WriteFile(dir.Path() / "2b47aab238f60515", MakeManifest({{kLevelRootType, kMplArenaA}}));
  const InstalledLevels result = CollectInstalledLevels(dir.Path(), {kSetNetClient, kSetNetClientLocale});
  EXPECT_EQ(result.levels, (std::vector<uint64_t>{kMplArenaA}));
  EXPECT_EQ(result.manifestsRead, 2u);
  EXPECT_TRUE(result.problems.empty());
}

TEST(InstalledLevels, AnUnreadableFileIsAProblemLineAndTheOthersStillCount) {
  ScratchDir dir;
  WriteFile(dir.Path() / "48037dc70b0ecab2", MakeManifest({{kLevelRootType, kMplArenaA}}));
  WriteFile(dir.Path() / "2b47aab238f60515", std::vector<uint8_t>{'j', 'u', 'n', 'k'});
  const InstalledLevels result = CollectInstalledLevels(dir.Path(), {kSetNetClient, kSetNetClientLocale});
  EXPECT_EQ(result.levels, (std::vector<uint64_t>{kMplArenaA}));
  EXPECT_EQ(result.manifestsRead, 1u);
  ASSERT_EQ(result.problems.size(), 1u);
  EXPECT_EQ(result.problems[0], "2b47aab238f60515: not a ZSTD manifest");
}

TEST(InstalledLevels, BootSetsAndLevelTypeAreTheNamedSymbols) {
  EXPECT_EQ(BootPackageSets(), (std::vector<uint64_t>{kSetNetClient, kSetNetClientLocale}));
  EXPECT_EQ(EchoVR::CSymbol64Hash("CGameLevelResourceWin10"), kLevelRootType);
  EXPECT_EQ(EchoVR::CSymbol64Hash("mpl_arena_a"), kMplArenaA);
}

TEST(InstalledLevels, FindsEveryDataVersionsManifestFolderAndSkipsTheRest) {
  ScratchDir dir;
  const std::filesystem::path exe = dir.Path() / "echovr" / "bin" / "win10" / "echovr.exe";
  const std::filesystem::path data = dir.Path() / "echovr" / "_data";
  std::filesystem::create_directories(data / "5932408047" / "rad15" / "win10" / "manifests");
  std::filesystem::create_directories(data / "6000000000" / "rad15" / "win10" / "manifests");
  std::filesystem::create_directories(data / "7000000000" / "rad15" / "win10");  // no manifests folder
  const std::vector<std::filesystem::path> found = nevr_game_server::FindManifestDirs(exe);
  EXPECT_EQ(found, (std::vector<std::filesystem::path>{data / "5932408047" / "rad15" / "win10" / "manifests",
                                                       data / "6000000000" / "rad15" / "win10" / "manifests"}));
}

TEST(InstalledLevels, NoDataDirMeansNoManifestFolders) {
  ScratchDir dir;
  EXPECT_TRUE(nevr_game_server::FindManifestDirs(dir.Path() / "echovr" / "bin" / "win10" / "echovr.exe").empty());
}

TEST(InstalledLevels, AHeaderThatDisagreesWithTheFrameIsRefusedBeforeAnyAllocation) {
  // The container claims 200 MiB; the frame declares the real 5 MB body. The check refuses it, so no 200 MiB buffer exists.
  std::vector<uint8_t> file = MakeManifest({{kLevelRootType, kMplArenaA}});
  PutU64(file, 8, 200ULL * 1024 * 1024);
  std::vector<uint64_t> levels;
  std::string error;
  EXPECT_FALSE(ParseLevelRoots(file, &levels, &error));
  EXPECT_EQ(error, "frame declares a different size than the container");
}

TEST(InstalledLevels, AManifestOverTheFileSizeCapIsAProblemAndIsNotRead) {
  ScratchDir dir;
  WriteFile(dir.Path() / "48037dc70b0ecab2", {});
  std::filesystem::resize_file(dir.Path() / "48037dc70b0ecab2", 64ULL * 1024 * 1024 + 1);
  const InstalledLevels result = CollectInstalledLevels(dir.Path(), {kSetNetClient});
  EXPECT_EQ(result.manifestsRead, 0u);
  ASSERT_EQ(result.problems.size(), 1u);
  EXPECT_EQ(result.problems[0], "48037dc70b0ecab2: larger than the manifest size cap");
}

TEST(InstalledLevels, CollectionOverSeveralFoldersMergesThemWithoutDuplicates) {
  ScratchDir a;
  ScratchDir b;
  WriteFile(a.Path() / "48037dc70b0ecab2", MakeManifest({{kLevelRootType, kMplArenaA}}));
  WriteFile(b.Path() / "48037dc70b0ecab2", MakeManifest({{kLevelRootType, kMplArenaA}, {kLevelRootType, 0xBB}}));
  const InstalledLevels result = CollectInstalledLevelsFromDirs({a.Path(), b.Path()}, {kSetNetClient});
  EXPECT_EQ(result.levels, (std::vector<uint64_t>{0xBB, kMplArenaA}));
  EXPECT_EQ(result.manifestsRead, 2u);
}

TEST(InstalledLevels, ListsFollowTheServerDbParseNotATrim) {
  std::size_t rejected = 0;
  // " eu_west" has a space nakama's region pattern rejects; it is dropped, not trimmed.
  EXPECT_EQ(nevr_game_server::ParseServerDbList("us-east, eu_west,,us-east", nevr_game_server::ListItemKind::kRegion,
                                                &rejected),
            (std::vector<std::string>{"us-east", "us-east"}));
  EXPECT_EQ(rejected, 1u);

  EXPECT_EQ(nevr_game_server::ParseServerDbList("111,any,abc,222", nevr_game_server::ListItemKind::kGuild, &rejected),
            (std::vector<std::string>{"111", "222", "any"}));
  EXPECT_EQ(rejected, 1u);
}

TEST(InstalledLevels, AnItemIsCutTo32BytesBeforeThePatternIsChecked) {
  std::size_t rejected = 0;
  const std::string longRegion(40, 'r');
  const std::vector<std::string> items =
      nevr_game_server::ParseServerDbList(longRegion, nevr_game_server::ListItemKind::kRegion, &rejected);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0], std::string(32, 'r'));
  EXPECT_EQ(rejected, 0u);
}

// The registration still goes out when a manifest is corrupt: the reader reports a problem, and the envelope
// is built with the registration's own fields and no levels.
TEST(InstalledLevels, ACorruptManifestStillLetsTheRegistrationGoOut) {
  ScratchDir dir;
  std::vector<uint8_t> corrupt = MakeManifest({{kLevelRootType, kMplArenaA}});
  corrupt.resize(corrupt.size() / 2);  // a truncated frame
  WriteFile(dir.Path() / "48037dc70b0ecab2", corrupt);
  const InstalledLevels result = CollectInstalledLevels(dir.Path(), BootPackageSets());
  EXPECT_TRUE(result.levels.empty());
  EXPECT_EQ(result.problems.size(), 2u);  // the corrupt set and the absent locale set

  nevr_game_server::RegistrationParams params;
  params.loginSessionId = "11111111-2222-3333-4444-555555555555";
  params.serverId = 4242;
  params.installedLevels = result.levels;
  const gameservice::v1::Envelope envelope = nevr_game_server::BuildRegistrationEnvelope(params);
  ASSERT_EQ(envelope.message_case(), gameservice::v1::Envelope::kGameServerRegistration);
  EXPECT_EQ(envelope.game_server_registration().server_id(), 4242u);
  EXPECT_EQ(envelope.game_server_registration().installed_levels_size(), 0);
}

TEST(InstalledLevels, SplitsTheListShapedConfigValuesLikeTheUrlBuilder) {
  EXPECT_EQ(nevr_game_server::SplitCsvList("111,222"), (std::vector<std::string>{"111", "222"}));
  EXPECT_EQ(nevr_game_server::SplitCsvList("a,,b,"), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(nevr_game_server::SplitCsvList("us-east"), (std::vector<std::string>{"us-east"}));
  EXPECT_TRUE(nevr_game_server::SplitCsvList("").empty());
}

// Gated on a real manifests folder (a copy of the install). Expected on a stock PC install: 32 levels, from the
// two boot sets, with the locale set contributing none.
TEST(InstalledLevels, StockInstallHasThirtyTwoLevels) {
  const char* dir = std::getenv("NEVR_STOCK_MANIFEST_DIR");
  if (dir == nullptr || dir[0] == '\0') {
    GTEST_SKIP() << "NEVR_STOCK_MANIFEST_DIR is unset: no install to read";
  }
  const InstalledLevels result = CollectInstalledLevels(dir, BootPackageSets());
  EXPECT_TRUE(result.problems.empty()) << result.problems[0];
  EXPECT_EQ(result.manifestsRead, 2u);
  EXPECT_EQ(result.levels.size(), 32u);
}

TEST(InstalledLevels, PluginRegisteredLevelsJoinTheManifestLevelsWithoutDuplicates) {
  InstalledLevels levels;
  levels.levels = {kMplArenaA};
  AddRegisteredLevels({{kLevelRootType, 0xCAFE}, {kLevelRootType, kMplArenaA}, {0x1111111111111111ULL, 0x2222ULL}},
                      &levels);
  EXPECT_EQ(levels.levels, (std::vector<uint64_t>{0xCAFE, kMplArenaA}));  // sorted; the duplicate is kept once
  EXPECT_EQ(levels.pluginLevels, 2u);                                      // the other type is not a level
}

// The registry a plugin's NEVR_RegisterAsset call lands in is the source of these entries: this drives a real
// Registry through Register and feeds its Snapshot into the level list.
TEST(InstalledLevels, AssetsARegistryHoldsOfTheLevelTypeAreThePluginLevels) {
  const uint8_t payload[4] = {1, 2, 3, 4};
  nevr::assets::Registry registry;
  ASSERT_EQ(registry.Register(kLevelRootType, 0xCAFE, payload, sizeof(payload), "plugin-level"),
            nevr::assets::RegisterStatus::Ok);
  ASSERT_EQ(registry.Register(0x1111111111111111ULL, 0x2222ULL, payload, sizeof(payload), "plugin-texture"),
            nevr::assets::RegisterStatus::Ok);
  std::vector<std::pair<uint64_t, uint64_t>> keys;
  for (const nevr::assets::Asset& asset : registry.Snapshot()) keys.emplace_back(asset.type, asset.name);

  InstalledLevels levels;
  levels.levels = {kMplArenaA};
  AddRegisteredLevels(keys, &levels);
  EXPECT_EQ(levels.levels, (std::vector<uint64_t>{0xCAFE, kMplArenaA}));
  EXPECT_EQ(levels.pluginLevels, 1u);
}

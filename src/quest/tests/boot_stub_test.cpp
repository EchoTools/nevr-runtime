// Host tests for the zero-package stub manifests (src/quest/data/boot_stub.h): the bytes against the repository's own manifest
// reader (src/runtime/server/installed_levels.cpp, ParseLevelRoots and CollectInstalledLevels, which read the container and the
// resource array the way the engine's reader lays them out) and against libzstd, the paths against the pinned data set, and the
// data step's whole-game mode end to end on a real directory.

#include <zstd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/data/boot_stub.h"
#include "quest/data/data_step.h"
#include "quest/data/plan.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/mini_test.h"
#include "runtime/server/installed_levels.h"

using namespace nevr_quest::data;
using namespace data_test;

namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

std::uint64_t U64(const std::string& s, std::size_t at) {
  std::uint64_t v = 0;
  std::memcpy(&v, s.data() + at, sizeof(v));
  return v;
}
std::uint32_t U32(const std::string& s, std::size_t at) {
  std::uint32_t v = 0;
  std::memcpy(&v, s.data() + at, sizeof(v));
  return v;
}

// The body the file decompresses to, or empty when libzstd refuses the frame.
std::string Body(const std::string& file) {
  std::string body(0xc0, '\0');
  const std::size_t n = ZSTD_decompress(&body[0], body.size(), file.data() + 24, file.size() - 24);
  return ZSTD_isError(n) != 0 || n != body.size() ? std::string() : body;
}

bool RealExists(const char* path) noexcept {
  struct stat st;
  if (::stat(path, &st) != 0) return false;
  return S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && st.st_size > 0);
}

}  // namespace

TEST(TheStubContainerFieldsAreTheShippedOnes) {
  const std::string file = boot_stub::StubFileBytes();
  CHECK(file.size() > 24);
  CHECK_EQ(file.substr(0, 4), std::string("ZSTD"));
  CHECK_EQ(U32(file, 4), 0x10u);                    // alignment, as the shipped manifests
  CHECK_EQ(U64(file, 8), static_cast<std::uint64_t>(0xc0));  // decompressed length
  CHECK_EQ(U64(file, 16), static_cast<std::uint64_t>(file.size() - 24));  // compressed length: nonzero, or the engine raises
  CHECK_EQ(ZSTD_getFrameContentSize(file.data() + 24, file.size() - 24), static_cast<unsigned long long>(0xc0));
}

TEST(TheStubBodyHasNoPackageResourceFileOrChunkAndTheBlockSizeTheShippedOnesHave) {
  const std::string body = Body(boot_stub::StubFileBytes());
  CHECK_EQ(body.size(), static_cast<std::size_t>(0xc0));
  CHECK_EQ(U32(body, 0x00), 0u);        // package count: the engine opens no package file
  CHECK_EQ(U32(body, 0x04), 0x80000u);  // block size
  CHECK_EQ(U64(body, 0x38), static_cast<std::uint64_t>(0));  // resources
  CHECK_EQ(U64(body, 0x78), static_cast<std::uint64_t>(0));  // files
  CHECK_EQ(U64(body, 0xb8), static_cast<std::uint64_t>(0));  // chunks
  for (std::size_t i = 0; i < body.size(); ++i) {
    if (i >= 4 && i < 8) continue;
    if (body[i] != 0) {
      std::fprintf(stderr, "  body byte %zu is not zero\n", i);
      CHECK(false);
    }
  }
}

TEST(TheRepositorysManifestReaderAcceptsTheStubAndFindsNoLevel) {
  std::vector<std::uint64_t> levels;
  std::string error;
  CHECK(nevr_game_server::ParseLevelRoots(Bytes(boot_stub::StubFileBytes()), &levels, &error));
  CHECK(error.empty());
  CHECK(levels.empty());
}

TEST(TheReaderRefusesAStubThatIsCutShort) {
  std::string file = boot_stub::StubFileBytes();
  file.pop_back();
  std::vector<std::uint64_t> levels;
  std::string error;
  CHECK(!nevr_game_server::ParseLevelRoots(Bytes(file), &levels, &error));
  CHECK(!error.empty());
}

TEST(TheRepositorysReaderAcceptsAComputedFileOfAnAllZeroBodyToo) {
  // A COMPUTED example, not the shipped stub: 42 bytes, zstandard 0.25 compressing an all-zero 0xc0 body (block size 0, frame 18 bytes). It
  // came out of the disassembly read of libr15 `CPackageManifest::AddPackage` (0x2640904), which says the block size is not checked at count
  // 0; only its zstd decode was checked, in memory. No engine ran it. The shipped stub is a different file: 45 bytes, block size 0x80000,
  // frame 21 bytes, made with the same tool at level 3 (boot_stub.cpp). The engine has run neither; this test shows only that the reader
  // takes both.
  const char hex[] = "5a53544410000000c000000000000000120000000000000028b52ffd20c04d000010000001003b0a6001";
  std::string file;
  for (std::size_t i = 0; i + 1 < sizeof(hex); i += 2) file.push_back(static_cast<char>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
  CHECK_EQ(file.size(), static_cast<std::size_t>(42));
  std::vector<std::uint64_t> levels;
  std::string error;
  CHECK(nevr_game_server::ParseLevelRoots(Bytes(file), &levels, &error));
  CHECK(levels.empty());
}

TEST(TheSetsAreTheBootPackageSetsTheEngineAdds) {
  const std::vector<std::uint64_t> boot = nevr_game_server::BootPackageSets();
  CHECK_EQ(boot.size(), static_cast<std::size_t>(2));
  CHECK_EQ(boot[0], boot_stub::kSetIds[0]);
  CHECK_EQ(boot[1], boot_stub::kSetIds[1]);
  CHECK_EQ(boot_stub::kSetIds[0], EchoVR::CSymbol64Hash("r14netclient"));
  CHECK_EQ(boot_stub::kSetIds[1], EchoVR::CSymbol64Hash("r14netclient_en_US"));
  CHECK_EQ(boot_stub::SetFileName(boot_stub::kSetIds[0]), std::string("48037dc70b0ecab2"));
  CHECK_EQ(boot_stub::SetFileName(boot_stub::kSetIds[1]), std::string("2b47aab238f60515"));
}

TEST(TheStubPathIsTheStockManifestPathOfTheDataSetThatIsPinned) {
  CHECK_EQ(boot_stub::ManifestPath("/r", boot_stub::kSetIds[0]), std::string("/r/") + kManifestPath);
  CHECK_EQ(boot_stub::ManifestsDir("/r"), std::string("/r/_data/5932408047/rad15/android/manifests"));
}

TEST(TheWholeGameStepLeavesTwoManifestsTheRepositorysReaderCollectsAndNothingElse) {
  Scratch scratch;
  StepConfig config;
  config.exists = &RealExists;
  config.all_assets = true;
  config.fetch.backoff = [](int) {};  // were a fetch ever reached, no real sleep stands between a failure and the assertion
  config.fetch.max_attempts = 1;
  config.engine_root = scratch.Path("root");
  ::mkdir(config.engine_root.c_str(), 0755);
  ::mkdir((config.engine_root + "/_data").c_str(), 0755);  // so the engine root holds a `_data`, as after an `_data.off`
  std::vector<std::string> events;
  config.on_step = [&events](const Event& e) { events.push_back(e.name); };
  struct NoNet : Transport {
    int calls = 0;
    TransportResult GetRange(const std::string&, std::uint64_t, const BodyFn&, const KeepGoingFn&) override {
      ++calls;
      return TransportResult{};
    }
  } net;
  const StepResult r = RunDataStep(config, net);
  CHECK_EQ(r.outcome, StepOutcome::kStubbed);
  CHECK_EQ(net.calls, 0);
  const nevr_game_server::InstalledLevels found = nevr_game_server::CollectInstalledLevels(
      boot_stub::ManifestsDir(config.engine_root), nevr_game_server::BootPackageSets());
  CHECK_EQ(found.manifestsRead, static_cast<std::size_t>(2));
  CHECK(found.problems.empty());
  CHECK(found.levels.empty());
  // Only the two files: no package, no part, no staging directory.
  CHECK(!Exists(config.engine_root + "/_data/5932408047/rad15/android/packages"));
  CHECK(!Exists(config.engine_root + "/_data.zip.part"));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

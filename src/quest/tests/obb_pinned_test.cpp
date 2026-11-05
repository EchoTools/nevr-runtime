// The extractor on the real store OBB (docs/adr/0042): `obb_pinned_test <obb> <pins> <scratch>`, run by `just test-quest-obb-pinned`
// with NEVR_STORE_OBB. The expected values below are written out here, from the measurements in #2081, and are not read from
// the pin file: the test fails if the pin file and the measurements ever disagree.

#include <filesystem>
#include <string>

#include "quest/obb/byte_source.h"
#include "quest/obb/obb_footer.h"
#include "quest/obb/obb_key.h"
#include "quest/obb/stock_data.h"
#include "quest/tests/mini_test.h"

namespace fs = std::filesystem;
using namespace nevr_quest::obb;

namespace {
std::string g_obb, g_pins, g_scratch;

struct Expected {
  const char* path;
  const char* sha256;
  std::uint64_t size;
};
const Expected kTree[] = {
    {"_data/5932408047/rad15/android/manifests/48037dc70b0ecab2", "762c16ae58c9bc70eee071d8c0e2e8fc0408d1f0691032164029294c5cb7ec48", 828213},
    {"_data/5932408047/rad15/android/packages/48037dc70b0ecab2_0", "14d26efc718a8f8135039e47bd1321cbb556fc4181bf95ffcb5ef59c406a5a3b", 929620177},
    {"_data/5932408047/rad15/android/manifests/2b47aab238f60515", "26becc4814f88436b522a36318463eff60c15e4337f0912d7f21237b17754ebe", 1461},
    {"_data/5932408047/rad15/android/packages/2b47aab238f60515_0", "dd0a1087acfe15b0563c17298963b707f949ad4efb42d0ae85b8faa0b4b47e12", 14995192},
};
const char kObbSha256[] = "8020d1791c8806b3c9592806d2476c9b9c9771cbfcbce112914b357b66180607";
}  // namespace

TEST(TheStoreObbFooterIsTheMeasuredOne) {
  FileSource obb;
  CHECK(obb.Open(g_obb).ok);
  CHECK_EQ(obb.Size(), 946094131ull);
  ObbFooter footer;
  const Status s = ParseObbFooter(obb, &footer);
  CHECK(s.ok);
  CHECK_EQ(footer.footer_version, 1u);
  CHECK_EQ(footer.package_version, 1u);
  CHECK_EQ(footer.flags, 2u);
  const std::uint8_t salt[8] = {0xdf, 0xc9, 0x0a, 0x5f, 0x1b, 0xbe, 0x68, 0x35};
  CHECK(std::equal(footer.salt.begin(), footer.salt.end(), salt));
  CHECK_EQ(footer.package_name, std::string("com.readyatdawn.r15"));
  CHECK_EQ(footer.image_bytes, 946094080ull);
  CHECK_EQ(footer.image_bytes / 512, 1847840ull);
}

TEST(TheStoreObbExtractsToThePinnedTree) {
  fs::remove_all(g_scratch);
  fs::create_directories(g_scratch);
  StockDataOptions options;
  options.obb_path = g_obb;
  options.pins_path = g_pins;
  options.cache_root = g_scratch;
  BuildLog log([](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); });
  StockDataResult first;
  const Status s = BuildStockData(options, log, &first);
  CHECK(s.ok);
  if (!s.ok) std::fprintf(stderr, "  refused: %s: %s\n", s.code.c_str(), s.detail.c_str());
  CHECK_EQ(first.obb_sha256, std::string(kObbSha256));
  CHECK(!first.cache_hit);
  CHECK_EQ(first.files.size(), 4u);
  for (const Expected& e : kTree) {
    std::string sha;
    std::uint64_t size = 0;
    CHECK(HashFile(first.directory + "/" + e.path, &sha, &size).ok);  // from the disk, not from the step's own report
    CHECK_EQ(sha, std::string(e.sha256));
    CHECK_EQ(size, e.size);
  }
  StockDataResult second;
  CHECK(BuildStockData(options, log, &second).ok);
  CHECK(second.cache_hit);
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: obb_pinned_test <store obb> <pins file> <scratch dir>\n");
    return 2;
  }
  g_obb = argv[1];
  g_pins = argv[2];
  g_scratch = argv[3];
  return mini_test::RunAll(1, argv, std::chrono::seconds(300));
}

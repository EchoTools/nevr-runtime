// The PC's stock source (runtime/cargo/stock_source.h over the shared core/stock_source.h; docs/adr/0048, the recipes) over a
// synthetic install in a temp folder: manifests and packages the test writes in the game's layout
// (docs/reference/package-manifest-format.md). Never game files.
#include "runtime/cargo/stock_source.h"

#include <gtest/gtest.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>
#include <zstd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "runtime/server/installed_levels.h"

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint64_t kType = 0x1111222233334444ULL;

Bytes Pattern(std::size_t size, unsigned mul, unsigned add) {
  Bytes b(size);
  for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(i * mul + add);
  return b;
}

void Put(Bytes* b, std::size_t at, unsigned width, std::uint64_t v) {
  if (b->size() < at + width) b->resize(at + width);
  for (unsigned i = 0; i < width; ++i) (*b)[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

Bytes Compress(const Bytes& in) {
  Bytes out(ZSTD_compressBound(in.size()));
  out.resize(ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3));
  return out;
}

std::string Hex16(std::uint64_t v) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
  return text;
}

void WriteFile(const fs::path& path, const Bytes& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct Res {
  std::uint64_t name;
  std::uint32_t chunk, offset, size;
};
struct Chk {
  std::uint32_t offset, compressed, decompressed;
};

// A manifest container around a body with `resources` and `chunks` in one package.
Bytes Manifest(const std::vector<Res>& resources, const std::vector<Chk>& chunks) {
  Bytes body(0xc0, 0);
  Put(&body, 0x00, 4, 1);
  Put(&body, 0x04, 4, 0x80000);
  Put(&body, 0x38, 8, resources.size());
  Put(&body, 0xb8, 8, chunks.size());
  for (const Res& r : resources) {
    const std::size_t at = body.size();
    Put(&body, at, 8, kType);
    Put(&body, at + 8, 8, r.name);
    Put(&body, at + 0x10, 4, r.chunk);
    Put(&body, at + 0x14, 4, r.offset);
    Put(&body, at + 0x18, 4, r.size);
    Put(&body, at + 0x1c, 4, 16);
  }
  for (const Chk& c : chunks) {
    const std::size_t at = body.size();
    Put(&body, at, 4, 0);
    Put(&body, at + 4, 4, c.offset);
    Put(&body, at + 8, 4, c.compressed);
    Put(&body, at + 12, 4, c.decompressed);
  }
  const Bytes frame = Compress(body);
  Bytes file = {'Z', 'S', 'T', 'D'};
  Put(&file, 4, 4, 0x10);
  Put(&file, 8, 8, body.size());
  Put(&file, 16, 8, frame.size());
  file.insert(file.end(), frame.begin(), frame.end());
  return file;
}

class StockSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t temp[MAX_PATH];
    ASSERT_GT(GetTempPathW(MAX_PATH, temp), 0u);
    root_ = fs::path(temp) / (L"nevr_stock_source_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(GetTickCount64()));
    const fs::path win10 = root_ / L"_data" / L"5932408047" / L"rad15" / L"win10";
    set_ = Hex16(nevr_game_server::BootPackageSets()[0]);
    manifest_ = win10 / L"manifests" / set_;
    package_ = win10 / L"packages" / (set_ + "_0");
    fs::create_directories(root_ / L"bin" / L"win10");

    // Chunk 0 packs two small resources; chunk 1 is one resource over the block size; then the end sentinel.
    small_ = Pattern(300, 7, 1);
    large_ = Pattern(600000, 13, 5);
    const Bytes f0 = Compress(small_), f1 = Compress(large_);
    Bytes package = f0;
    package.insert(package.end(), f1.begin(), f1.end());
    WriteFile(package_, package);
    chunks_ = {{0, static_cast<std::uint32_t>(f0.size()), 300},
               {static_cast<std::uint32_t>(f0.size()), static_cast<std::uint32_t>(f1.size()), 600000},
               {static_cast<std::uint32_t>(package.size()), 0, 0}};
    WriteFile(manifest_, Manifest({{1, 0, 0, 100}, {2, 0, 100, 200}, {3, 1, 0, 600000}}, chunks_));
    source_ = nevr_maps::MakeWinStockSource(root_.wstring());
  }
  void TearDown() override {
    source_.reset();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  fs::path root_, manifest_, package_;
  std::string set_;
  std::vector<Chk> chunks_;
  Bytes small_, large_;
  std::unique_ptr<nevr_maps::StockSource> source_;
};

TEST_F(StockSourceTest, ReadsAResourceInsideAPackedChunk) {
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  ASSERT_TRUE(source_->ReadResource(kType, 2, &out, &read_from));
  EXPECT_EQ(out, Bytes(small_.begin() + 100, small_.end()));
  ASSERT_EQ(read_from.size(), 2u);
  EXPECT_EQ(fs::u8path(read_from[0].path), manifest_);
  EXPECT_EQ(fs::u8path(read_from[1].path), package_);
  EXPECT_EQ(read_from[1].stamp.size, fs::file_size(package_));
}

TEST_F(StockSourceTest, ReadsALargeResourceStraightFromItsChunk) {
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  ASSERT_TRUE(source_->ReadResource(kType, 3, &out, &read_from));
  EXPECT_EQ(out, large_);
}

TEST_F(StockSourceTest, AKeyTheInstallLacksIsFalse) {
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  EXPECT_FALSE(source_->ReadResource(kType, 99, &out, &read_from));
  EXPECT_FALSE(source_->ReadResource(kType + 1, 2, &out, &read_from));
  EXPECT_TRUE(out.empty());
}

TEST_F(StockSourceTest, ReadsWhileTheGameHoldsThePackageForWritingAndHoldsNothingAfter) {
  const HANDLE writer = CreateFileW(package_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(writer, INVALID_HANDLE_VALUE);
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  EXPECT_TRUE(source_->ReadResource(kType, 1, &out, &read_from));
  CloseHandle(writer);
  // An exclusive open succeeds only when no handle to the file is left open.
  for (const fs::path& path : {package_, manifest_}) {
    const HANDLE exclusive = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(exclusive, INVALID_HANDLE_VALUE) << path;
    if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
  }
}

TEST_F(StockSourceTest, StampsTurnStaleWhenAFileChangesOrGoes) {
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  ASSERT_TRUE(source_->ReadResource(kType, 2, &out, &read_from));
  EXPECT_TRUE(source_->StampsCurrent(read_from));
  {
    std::ofstream append(package_, std::ios::binary | std::ios::app);
    append.put('\0');
  }
  EXPECT_FALSE(source_->StampsCurrent(read_from));
  read_from.clear();
  ASSERT_TRUE(source_->ReadResource(kType, 2, &out, &read_from));
  EXPECT_TRUE(source_->StampsCurrent(read_from));
  fs::remove(package_);
  EXPECT_FALSE(source_->StampsCurrent(read_from));
}

TEST_F(StockSourceTest, AChangedManifestIsReadAgain) {
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  ASSERT_TRUE(source_->ReadResource(kType, 2, &out, &read_from));
  WriteFile(manifest_, Manifest({{1, 0, 0, 100}, {3, 1, 0, 600000}}, chunks_));
  EXPECT_FALSE(source_->ReadResource(kType, 2, &out, &read_from));
  EXPECT_TRUE(source_->ReadResource(kType, 1, &out, &read_from));
}

TEST_F(StockSourceTest, AManifestThatDoesNotParseServesNothing) {
  Bytes bad = Manifest({{1, 0, 0, 100}}, chunks_);
  bad[0] = 'X';
  WriteFile(manifest_, bad);
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  EXPECT_FALSE(source_->ReadResource(kType, 1, &out, &read_from));
  WriteFile(manifest_, Manifest({{1, 0, 0, 400}, {2, 0, 0, 10}}, chunks_));  // runs past its chunk; another size, so re-read
  EXPECT_FALSE(source_->ReadResource(kType, 1, &out, &read_from));
}

TEST_F(StockSourceTest, ReadsAStockScriptByHash) {
  const Bytes dll = Pattern(5000, 3, 9);
  WriteFile(root_ / L"bin" / L"win10" / L"scripts" / L"00000000000000ab.dll", dll);
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  ASSERT_TRUE(source_->ReadScript(0xab, &out, &read_from));
  EXPECT_EQ(out, dll);
  ASSERT_EQ(read_from.size(), 1u);
  EXPECT_TRUE(source_->StampsCurrent(read_from));
  EXPECT_FALSE(source_->ReadScript(0xac, &out, &read_from));
}

TEST(StockSourceNoInstall, AnEmptyFolderHoldsNothing) {
  const auto source = nevr_maps::MakeWinStockSource(L"C:\\nevr-stock-source-test-does-not-exist");
  Bytes out;
  std::vector<nevr_maps::StockStamp> read_from;
  EXPECT_FALSE(source->ReadResource(kType, 1, &out, &read_from));
  EXPECT_FALSE(source->ReadScript(1, &out, &read_from));
  EXPECT_TRUE(read_from.empty());
  EXPECT_TRUE(source->StampsCurrent({}));
}

}  // namespace

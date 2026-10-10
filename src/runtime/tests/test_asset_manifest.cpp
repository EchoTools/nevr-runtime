#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "runtime/patch/asset_manifest.h"
#include "runtime/tests/asset_manifest_fixture.h"

using nevr::assets::Chunk;
using nevr::assets::Decode;
using nevr::assets::Encode;
using nevr::assets::FileEntry;
using nevr::assets::Manifest;
using nevr::assets::ResourceEntry;

namespace {

uint64_t Rd64(const std::vector<uint8_t>& b, size_t off) {
    uint64_t v = 0;
    std::memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

uint32_t Rd32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, sizeof(v));
    return v;
}

Manifest Sample() {
    Manifest m;
    m.packageCount = 1;
    m.blockSize = 0x1000;
    m.resources.push_back({0xbeac1969cb7b8861ULL, 0x1111222233334444ULL, 0, 7, 0x100, 9});
    m.resources.push_back({0xbeac1969cb7b8861ULL, 0x5555666677778888ULL, 1, 0, 0x40, 0});
    FileEntry f;
    for (int i = 0; i < 5; ++i) f.word[i] = 0xa0 + i;
    m.files.push_back(f);
    m.files.push_back(f);
    m.chunks.push_back({0x0, 0x10});
    m.chunks.push_back({0x1000, 0x20});
    m.chunks.push_back({0x2000, 0x30});
    return m;
}

}  // namespace

// The shipped manifest is the ground truth: the engine's reader consumed it, so a decoder that disagrees about
// where the arrays sit would not account for every byte.
TEST(AssetManifest, DecodesShippedManifestAccountingForEveryByte) {
    Manifest m;
    std::string err;
    ASSERT_TRUE(Decode(kShippedSmallManifest, kShippedSmallManifestSize, &m, &err)) << err;
    EXPECT_EQ(m.packageCount, 1u);
    EXPECT_EQ(m.blockSize, 0x80000u);
    EXPECT_EQ(m.resources.size(), 43u);
    EXPECT_EQ(m.files.size(), 43u);
    EXPECT_EQ(m.chunks.size(), 21u);
    // First record, read by eye from the file: DDS texture type, name 0x9f118d37013acde8, chunk 10, size 0x100094.
    EXPECT_EQ(m.resources[0].type, 0xbeac1969cb7b8861ULL);
    EXPECT_EQ(m.resources[0].name, 0x9f118d37013acde8ULL);
    EXPECT_EQ(m.resources[0].chunk, 0xau);
    EXPECT_EQ(m.resources[0].size, 0x100094u);
}

TEST(AssetManifest, ShippedManifestReEncodesToTheSameContent) {
    Manifest m;
    ASSERT_TRUE(Decode(kShippedSmallManifest, kShippedSmallManifestSize, &m, nullptr));
    const std::vector<uint8_t> again = Encode(m);
    Manifest m2;
    ASSERT_TRUE(Decode(again.data(), again.size(), &m2, nullptr));
    ASSERT_EQ(m2.resources.size(), m.resources.size());
    for (size_t i = 0; i < m.resources.size(); ++i) {
        EXPECT_EQ(m2.resources[i].type, m.resources[i].type);
        EXPECT_EQ(m2.resources[i].name, m.resources[i].name);
        EXPECT_EQ(m2.resources[i].chunk, m.resources[i].chunk);
        EXPECT_EQ(m2.resources[i].size, m.resources[i].size);
    }
    // Everything after the 0xc0 header is the arrays back to back; only capacity words may differ in the header.
    ASSERT_EQ(again.size(), kShippedSmallManifestSize);
    EXPECT_EQ(0, std::memcmp(again.data() + nevr::assets::kHeaderBytes,
                             kShippedSmallManifest + nevr::assets::kHeaderBytes,
                             kShippedSmallManifestSize - nevr::assets::kHeaderBytes));
}

TEST(AssetManifest, EncodedHeaderMirrorsTheEnginesStruct) {
    const std::vector<uint8_t> bytes = Encode(Sample());
    EXPECT_EQ(Rd32(bytes, 0x00), 1u);        // packageCount
    EXPECT_EQ(Rd32(bytes, 0x04), 0x1000u);   // blockSize
    EXPECT_EQ(Rd64(bytes, 0x38), 2u);        // resources
    EXPECT_EQ(Rd64(bytes, 0x78), 2u);        // files
    EXPECT_EQ(Rd64(bytes, 0xb8), 3u);        // chunks
    EXPECT_EQ(Rd32(bytes, 0x10), 2u * 0x20); // capacity bytes, resources
    EXPECT_EQ(Rd32(bytes, 0x50), 2u * 0x28); // capacity bytes, files
    EXPECT_EQ(Rd32(bytes, 0x90), 3u * 0x10); // capacity bytes, chunks
    EXPECT_EQ(bytes.size(), 0xc0u + 2u * 0x20 + 2u * 0x28 + 3u * 0x10);
}

TEST(AssetManifest, RoundTripsEveryField) {
    const Manifest in = Sample();
    const std::vector<uint8_t> bytes = Encode(in);
    Manifest out;
    std::string err;
    ASSERT_TRUE(Decode(bytes.data(), bytes.size(), &out, &err)) << err;
    EXPECT_EQ(out.packageCount, in.packageCount);
    EXPECT_EQ(out.blockSize, in.blockSize);
    ASSERT_EQ(out.resources.size(), 2u);
    EXPECT_EQ(out.resources[0].a, 7u);
    EXPECT_EQ(out.resources[0].b, 9u);
    EXPECT_EQ(out.resources[1].name, 0x5555666677778888ULL);
    ASSERT_EQ(out.files.size(), 2u);
    EXPECT_EQ(out.files[1].word[4], 0xa4u);
    ASSERT_EQ(out.chunks.size(), 3u);
    EXPECT_EQ(out.chunks[2].second, 0x30u);
}

TEST(AssetManifest, EmptyManifestIsHeaderOnly) {
    Manifest m;
    const std::vector<uint8_t> bytes = Encode(m);
    EXPECT_EQ(bytes.size(), nevr::assets::kHeaderBytes);
    Manifest out;
    EXPECT_TRUE(Decode(bytes.data(), bytes.size(), &out, nullptr));
}

TEST(AssetManifest, RejectsShortInput) {
    const std::vector<uint8_t> bytes = Encode(Sample());
    Manifest out;
    std::string err;
    EXPECT_FALSE(Decode(bytes.data(), nevr::assets::kHeaderBytes - 1, &out, &err));
    EXPECT_FALSE(err.empty());
}

TEST(AssetManifest, RejectsArraysThatOverrunTheInput) {
    std::vector<uint8_t> bytes = Encode(Sample());
    bytes.resize(bytes.size() - 1);
    Manifest out;
    std::string err;
    EXPECT_FALSE(Decode(bytes.data(), bytes.size(), &out, &err));
    EXPECT_NE(err.find("overrun"), std::string::npos) << err;
}

TEST(AssetManifest, RejectsTrailingBytesTheCountsDoNotAccountFor) {
    std::vector<uint8_t> bytes = Encode(Sample());
    bytes.push_back(0);
    Manifest out;
    std::string err;
    EXPECT_FALSE(Decode(bytes.data(), bytes.size(), &out, &err));
    EXPECT_NE(err.find("trailing"), std::string::npos) << err;
}

TEST(AssetManifest, RejectsBlockSizeTheEngineWouldRefuse) {
    Manifest m = Sample();
    m.blockSize = nevr::assets::kMaxBlockSize + 1;
    const std::vector<uint8_t> bytes = Encode(m);
    Manifest out;
    std::string err;
    EXPECT_FALSE(Decode(bytes.data(), bytes.size(), &out, &err));
    EXPECT_NE(err.find("block size"), std::string::npos) << err;
}

TEST(AssetManifest, RejectsCountsThatCannotFitInTheInput) {
    std::vector<uint8_t> bytes = Encode(Sample());
    // A hostile count of 2^60 resources must not wrap the size arithmetic into something that "fits".
    const uint64_t huge = 1ULL << 60;
    std::memcpy(bytes.data() + 0x38, &huge, sizeof(huge));
    Manifest out;
    std::string err;
    EXPECT_FALSE(Decode(bytes.data(), bytes.size(), &out, &err));
    EXPECT_NE(err.find("overrun"), std::string::npos) << err;
}

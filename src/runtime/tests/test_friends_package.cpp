// #1535 / #1599: the FRIENDS panel's own package set (friends_package.h). The written manifest and package are read
// back here with zstd and the documented layout; the stock texture the format is checked against (game data, not in
// the repository) is read from NEVR_PCVR_EXTRACTED (<dir>/<on-disk type hex>/<name hex>) when it is set.

#include <gtest/gtest.h>

#include <zstd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "runtime/patch/friends_package.h"

namespace {

namespace fp = nevr_friends_package;
using fp::Bytes;

template <typename T>
T Get(const Bytes& b, std::size_t at) {
  T v{};
  std::memcpy(&v, b.data() + at, sizeof(v));
  return v;
}

Bytes Decompress(const std::uint8_t* data, std::size_t size, std::size_t raw) {
  Bytes out(raw);
  const std::size_t n = ZSTD_decompress(out.data(), out.size(), data, size);
  EXPECT_FALSE(ZSTD_isError(n));
  out.resize(ZSTD_isError(n) ? 0 : n);
  return out;
}

// The resources a set holds, by (type, name), read through its manifest.
std::map<std::pair<std::uint64_t, std::uint64_t>, Bytes> ReadSet(const fp::PackageSet& set, Bytes* manifest_out) {
  std::map<std::pair<std::uint64_t, std::uint64_t>, Bytes> out;
  const Bytes& f = set.manifest;
  EXPECT_EQ(std::memcmp(f.data(), "ZSTD", 4), 0);
  EXPECT_EQ(Get<std::uint32_t>(f, 4), 16u);
  const auto raw = Get<std::uint64_t>(f, 8), compressed = Get<std::uint64_t>(f, 16);
  EXPECT_EQ(24 + compressed, f.size());
  const Bytes m = Decompress(f.data() + 24, compressed, raw);
  *manifest_out = m;
  const auto n = Get<std::uint64_t>(m, 0x30), nb = Get<std::uint64_t>(m, 0xB0);
  const std::size_t res = 0xC0, blocks = res + 32 * n + 40 * n;
  EXPECT_EQ(m.size(), blocks + 16 * nb);
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::size_t r = res + 32 * i;
    const auto block = Get<std::uint32_t>(m, r + 16), offset = Get<std::uint32_t>(m, r + 20), size = Get<std::uint32_t>(m, r + 24);
    const std::size_t b = blocks + 16 * block;
    const Bytes data = Decompress(set.package.data() + Get<std::uint32_t>(m, b + 4), Get<std::uint32_t>(m, b + 8),
                                  Get<std::uint32_t>(m, b + 12));
    out[{Get<std::uint64_t>(m, r), Get<std::uint64_t>(m, r + 8)}] = Bytes(data.begin() + offset, data.begin() + offset + size);
  }
  return out;
}

}  // namespace

TEST(FriendsPackage, DdsIsAOneMipB8G8R8A8Image) {
  const Bytes pixels(8 * 4 * 4, 0x7F);
  const Bytes dds = fp::Dds(8, 4, pixels);
  ASSERT_EQ(dds.size(), 148u + pixels.size());
  EXPECT_EQ(std::memcmp(dds.data(), "DDS ", 4), 0);
  EXPECT_EQ(Get<std::uint32_t>(dds, 12), 4u);   // height
  EXPECT_EQ(Get<std::uint32_t>(dds, 16), 8u);   // width
  EXPECT_EQ(Get<std::uint32_t>(dds, 20), 32u);  // row pitch
  EXPECT_EQ(std::memcmp(dds.data() + 84, "DX10", 4), 0);
  EXPECT_EQ(Get<std::uint32_t>(dds, 128), fp::kDxgiB8G8R8A8);
  EXPECT_EQ(dds[148], 0x7F);
}

TEST(FriendsPackage, DescriptorNamesSizeFormatAndPayload) {
  const Bytes d = fp::TextureDescriptor(128, 128, 65684);
  ASSERT_EQ(d.size(), 256u);
  for (std::size_t i = 0; i < 192; ++i) ASSERT_EQ(d[i], 0xFF) << i;
  EXPECT_EQ(Get<std::uint32_t>(d, 196), 128u);
  EXPECT_EQ(Get<std::uint32_t>(d, 216), 87u);
  EXPECT_EQ(Get<std::uint32_t>(d, 244), 65684u);
  EXPECT_EQ(Get<std::uint32_t>(d, 248), 512u);
}

TEST(FriendsPackage, ASetReadsBackToItsResourcesInSignedOrder) {
  std::vector<fp::Resource> in = {{0x8000000000000001ULL, 2, Bytes(10, 1)}, {1, 0x8000000000000000ULL, Bytes(700000, 2)},
                                  {1, 5, Bytes(3, 3)}};
  fp::PackageSet set;
  ASSERT_TRUE(fp::BuildPackageSet(in, &set));
  Bytes m;
  const auto got = ReadSet(set, &m);
  ASSERT_EQ(got.size(), 3u);
  for (const auto& r : in) EXPECT_EQ(got.at({r.type, r.name}), r.bytes);
  // Records in signed (type, name) order: the negative type first, then type 1 with the negative name.
  EXPECT_EQ(Get<std::uint64_t>(m, 0xC0), 0x8000000000000001ULL);
  EXPECT_EQ(Get<std::uint64_t>(m, 0xC0 + 32 + 8), 0x8000000000000000ULL);
  // The 700000-byte resource is larger than a block: it has one of its own, so there are 3 blocks + 2 end records.
  EXPECT_EQ(Get<std::uint64_t>(m, 0xB0), 5u);
  EXPECT_EQ(Get<std::uint32_t>(m, 4), fp::kBlockSize);
}

TEST(FriendsPackage, RefusesAnEmptyOrRepeatedResource) {
  fp::PackageSet set;
  EXPECT_FALSE(fp::BuildPackageSet({{1, 1, Bytes()}}, &set));
  EXPECT_FALSE(fp::BuildPackageSet({{1, 1, Bytes(1, 0)}, {1, 1, Bytes(2, 0)}}, &set));
}

TEST(FriendsPackage, ThePicturesSetHasAPlaceholderForEveryName) {
  fp::PackageSet set;
  ASSERT_TRUE(fp::BuildFriendsPictures(fp::kWin10, 5, 3, &set));
  Bytes m;
  const auto got = ReadSet(set, &m);
  EXPECT_EQ(got.size(), 2u * (5 * 3 + 3));
  const Bytes& payload = got.at({fp::kTexturePayloadType, fp::RowPicture(4, 2)});
  ASSERT_EQ(payload.size(), 148u + 128u * 128u * 4u);
  EXPECT_EQ(payload[148], fp::kPlaceholderBgra[0]);
  EXPECT_EQ(got.at({fp::kTextureDescriptorType, fp::ProfilePicture(0)}), fp::TextureDescriptor(128, 128, 65684));
  EXPECT_EQ(fp::RowPicture(0, 0), EchoVR::CSymbol64Hash("nevr_friend_avatar_0_a"));
}

TEST(FriendsPackage, AQuestPlaceholderIsSolidAstcBlocksWithTheQuestDescriptor) {
  fp::PackageSet set;
  ASSERT_TRUE(fp::BuildFriendsPictures(fp::kAndroid, 5, 3, &set));
  Bytes m;
  const auto got = ReadSet(set, &m);
  EXPECT_EQ(got.size(), 2u * (5 * 3 + 3));
  const Bytes& payload = got.at({fp::kAndroid.payload_type, fp::RowPicture(2, 1)});
  ASSERT_EQ(payload.size(), 4096u);  // 16 x 16 blocks of 16 bytes: 128 x 128 at 8 x 8
  const auto block = fp::AstcSolidBlock(fp::kPlaceholderBgra[2], fp::kPlaceholderBgra[1], fp::kPlaceholderBgra[0], 0xFF);
  for (std::size_t at = 0; at < payload.size(); at += 16)
    ASSERT_EQ(std::memcmp(payload.data() + at, block.data(), 16), 0) << at;
  EXPECT_EQ(got.at({fp::kAndroid.descriptor_type, fp::ProfilePicture(0)}), fp::AndroidTextureDescriptor(128, 128, 4096));
}

TEST(FriendsPackage, AnAstcSolidBlockIsAnLdrVoidExtentOfTheColour) {
  const auto block = fp::AstcSolidBlock(0x33, 0x3D, 0x49, 0xFF);
  std::uint64_t low = 0;
  std::memcpy(&low, block.data(), 8);
  EXPECT_EQ(low & 0x1FF, 0x1FCu);       // block mode: void extent
  EXPECT_EQ((low >> 9) & 1, 0u);        // LDR
  EXPECT_EQ(low >> 12, 0xFFFFFFFFFFFFFULL);  // no extent: the whole texture
  std::uint16_t rgba[4];
  std::memcpy(rgba, block.data() + 8, 8);
  EXPECT_EQ(rgba[0], 0x3333u);
  EXPECT_EQ(rgba[2], 0x4949u);
  EXPECT_EQ(rgba[3], 0xFFFFu);
}

// The Quest's stock 128 x 128 texture (the same name as PC's): its descriptor is exactly what AndroidTextureDescriptor
// writes, and its payload is one 128 x 128 image at 8 x 8 blocks. NEVR_QUEST_TEXTURES names a directory of
// <on-disk type hex>/<name hex> files extracted from the headset's own data.
TEST(FriendsPackage, AQuestStockTextureIsWhatTheQuestWriterWrites) {
  const char* dir = std::getenv("NEVR_QUEST_TEXTURES");
  if (dir == nullptr || *dir == '\0') GTEST_SKIP() << "NEVR_QUEST_TEXTURES not set";
  const auto read = [dir](const char* type, const char* name) {
    std::ifstream in(std::string(dir) + "/" + type + "/" + name, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  EXPECT_EQ(read("e2efe7289d5985b8", "84b9a06506af66fe"), fp::AndroidTextureDescriptor(128, 128, 4096));
  EXPECT_EQ(read("489bb35d53ca50e9", "84b9a06506af66fe").size(), 4096u);
}

// The stock 128 x 128 B8G8R8A8 texture's descriptor and DDS header are exactly what TextureDescriptor and Dds write.
TEST(FriendsPackage, AStockTextureIsWhatTheWritersWrite) {
  const char* dir = std::getenv("NEVR_PCVR_EXTRACTED");
  if (dir == nullptr || *dir == '\0') GTEST_SKIP() << "NEVR_PCVR_EXTRACTED not set";
  const auto read = [dir](const char* type, const char* name) {
    std::ifstream in(std::string(dir) + "/" + type + "/" + name, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const Bytes descriptor = read("4a4c32c49300b8a0", "84b9a06506af66fe");
  const Bytes payload = read("beac1969cb7b8861", "84b9a06506af66fe");
  ASSERT_EQ(payload.size(), 148u + 65536u);
  EXPECT_EQ(descriptor, fp::TextureDescriptor(128, 128, 148 + 65536));
  const Bytes mine = fp::Dds(128, 128, Bytes(65536, 0));
  EXPECT_EQ(Bytes(payload.begin(), payload.begin() + 148), Bytes(mine.begin(), mine.begin() + 148));
}

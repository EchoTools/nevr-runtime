// Host tests for the OBB image cipher (src/quest/obb/obb_cipher.cpp, docs/adr/0042): Twofish-128 CBC with dm-crypt's plain IV,
// checked against an image encrypted here with nettle's own encrypt side.

#include <cstring>

#include "quest/obb/byte_source.h"
#include "quest/obb/obb_cipher.h"
#include "quest/tests/mini_test.h"
#include "quest/tests/obb_test_support.h"

using namespace nevr_quest::obb;
using namespace obb_test;

namespace {
const std::uint8_t kSalt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
VolumeKey KeyOf(const std::vector<std::uint8_t>& k) {
  VolumeKey key;
  std::copy(k.begin(), k.end(), key.begin());
  return key;
}
}  // namespace

TEST(WholeImageDecryptsToThePlaintext) {
  // 300 sectors: sector numbers above 255 need the second byte of the IV.
  const std::vector<std::uint8_t> plain = Pattern(300 * 512, 5);
  const std::vector<std::uint8_t> ref_key = ReferenceKey("k", kSalt);
  MemorySource encrypted(EncryptImage(plain, ref_key));
  DecryptingSource image(encrypted, KeyOf(ref_key), plain.size());
  std::vector<std::uint8_t> got(plain.size());
  const Status s = image.ReadAt(0, got.data(), got.size());
  CHECK(s.ok);
  CHECK(got == plain);
}

TEST(SectorsAreChainedWithinThemselvesAndNotAcrossThem) {
  // Two identical plaintext sectors encrypt to different bytes (the IV is the sector number) and the second block of a
  // sector depends on the first (CBC), which the roundtrip above proves; here the ciphertext itself is checked.
  std::vector<std::uint8_t> plain(1024, 0);
  const std::vector<std::uint8_t> key = ReferenceKey("k", kSalt);
  const std::vector<std::uint8_t> enc = EncryptImage(plain, key);
  CHECK(!std::equal(enc.begin(), enc.begin() + 512, enc.begin() + 512));
  CHECK(!std::equal(enc.begin(), enc.begin() + 16, enc.begin() + 16));
}

TEST(UnalignedReadsAcrossSectorsAreExact) {
  const std::vector<std::uint8_t> plain = Pattern(6 * 512, 9);
  const std::vector<std::uint8_t> ref_key = ReferenceKey("k", kSalt);
  MemorySource encrypted(EncryptImage(plain, ref_key));
  DecryptingSource image(encrypted, KeyOf(ref_key), plain.size());
  std::vector<std::uint8_t> got(1500);
  CHECK(image.ReadAt(700, got.data(), got.size()).ok);
  CHECK(std::equal(got.begin(), got.end(), plain.begin() + 700));
  std::vector<std::uint8_t> one(1);
  CHECK(image.ReadAt(plain.size() - 1, one.data(), 1).ok);
  CHECK_EQ(one[0], plain.back());
  CHECK(image.ReadAt(plain.size(), one.data(), 0).ok);
}

TEST(ReadsBeyondTheImageAreRefused) {
  const std::vector<std::uint8_t> plain = Pattern(2 * 512, 1);
  const std::vector<std::uint8_t> ref_key = ReferenceKey("k", kSalt);
  MemorySource encrypted(EncryptImage(plain, ref_key));
  DecryptingSource image(encrypted, KeyOf(ref_key), plain.size());
  std::vector<std::uint8_t> got(16);
  CHECK_EQ(image.ReadAt(plain.size() - 8, got.data(), got.size()).code, std::string("source_short_read"));
  CHECK_EQ(image.ReadAt(plain.size() + 1, got.data(), 1).code, std::string("source_short_read"));
}

TEST(AnotherKeyDoesNotDecrypt) {
  const std::vector<std::uint8_t> plain = Pattern(2 * 512, 1);
  const std::vector<std::uint8_t> ref_key = ReferenceKey("k", kSalt);
  MemorySource encrypted(EncryptImage(plain, ref_key));
  DecryptingSource image(encrypted, KeyOf(ReferenceKey("other", kSalt)), plain.size());
  std::vector<std::uint8_t> got(plain.size());
  CHECK(image.ReadAt(0, got.data(), got.size()).ok);
  CHECK(got != plain);
}

TEST(EveryByteOfTheSectorNumberIsInTheIv) {
  // Sector 0x01020304 chains from the IV 04 03 02 01 00 ... : written out here, not derived from the tool's own code.
  const std::vector<std::uint8_t> plain = Pattern(512, 77);
  const std::vector<std::uint8_t> key = ReferenceKey("k", kSalt);
  twofish_ctx ctx;
  nettle_twofish_set_key(&ctx, key.size(), key.data());
  std::uint8_t iv[16] = {0x04, 0x03, 0x02, 0x01};
  std::vector<std::uint8_t> encrypted(512);
  nettle_cbc_encrypt(&ctx, reinterpret_cast<nettle_cipher_func*>(nettle_twofish_encrypt), 16, iv, 512, encrypted.data(), plain.data());
  TwofishCbcPlain cipher(KeyOf(key));
  std::vector<std::uint8_t> got(512);
  cipher.DecryptSector(0x01020304u, encrypted.data(), got.data());
  CHECK(got == plain);
  cipher.DecryptSector(0x01020305u, encrypted.data(), got.data());
  CHECK(got != plain);  // another sector number is another IV
}

namespace {
// A source that claims to be as large as asked and reads as zeros.
class ZeroSource final : public ByteSource {
 public:
  explicit ZeroSource(std::uint64_t size) : size_(size) {}
  std::uint64_t Size() const override { return size_; }
  Status ReadAt(std::uint64_t, void* dst, std::size_t length) const override {
    std::memset(dst, 0, length);
    return Status::Ok();
  }

 private:
  std::uint64_t size_;
};
}  // namespace

TEST(AnImageWithMoreSectorsThanA32BitNumberCountsIsRefused) {
  const std::uint64_t sectors = 0x100000001ull;
  ZeroSource huge(sectors * 512);
  VolumeKey key{};
  DecryptingSource image(huge, key, sectors * 512);
  std::uint8_t got[16];
  CHECK_EQ(image.ReadAt(0, got, sizeof got).code, std::string("obb_image_too_large"));
  const std::uint64_t fits = 0x100000000ull;  // exactly 2^32 sectors: the last sector number is 0xFFFFFFFF
  DecryptingSource edge(huge, key, fits * 512);
  CHECK(edge.ReadAt(0, got, sizeof got).ok);
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

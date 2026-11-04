// Host tests for the OBB image cipher (src/quest/obb/obb_cipher.cpp, docs/adr/0042): Twofish-128 CBC with dm-crypt's plain IV,
// checked against an image encrypted here with nettle's own encrypt side.

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

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

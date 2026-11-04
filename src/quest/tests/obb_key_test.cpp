// Host test for the OBB volume key (src/quest/obb/obb_key.cpp, docs/adr/0042): the store OBB's footer salt and the game's key
// string give the key the measured decryption used.

#include <string>

#include "quest/obb/obb_key.h"
#include "quest/tests/mini_test.h"
#include "quest/tests/obb_test_support.h"

using namespace nevr_quest::obb;

namespace {
std::string Hex(const VolumeKey& key) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (std::uint8_t b : key) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 15]);
  }
  return out;
}
const std::array<std::uint8_t, 8> kStoreSalt = {0xdf, 0xc9, 0x0a, 0x5f, 0x1b, 0xbe, 0x68, 0x35};
}  // namespace

TEST(StoreObbKeyIsTheMeasuredOne) {
  CHECK_EQ(Hex(DeriveVolumeKey(kGameObbKeyString, kStoreSalt)), std::string("329f4bd7978590e66cdfb8e1c20aeb58"));
}

TEST(TheGameKeyStringIsTheOneAtTheMountCall) {
  // libr15.so 0xf85434 passes this string to AStorageManager_mountObb (docs/adr/0007).
  CHECK_EQ(std::string(kGameObbKeyString), std::string("37c70a1635a1ad7a"));
}

TEST(KeyDependsOnSaltAndOnString) {
  std::array<std::uint8_t, 8> other = kStoreSalt;
  other[7] ^= 1;
  CHECK(Hex(DeriveVolumeKey(kGameObbKeyString, other)) != Hex(DeriveVolumeKey(kGameObbKeyString, kStoreSalt)));
  CHECK(Hex(DeriveVolumeKey("37c70a1635a1ad7b", kStoreSalt)) != Hex(DeriveVolumeKey(kGameObbKeyString, kStoreSalt)));
}

TEST(KeyIsNettlesPbkdf2At1024Rounds) {
  // The test support calls nettle's PBKDF2 directly; the tool's key is the same bytes.
  const std::vector<std::uint8_t> reference = obb_test::ReferenceKey(kGameObbKeyString, kStoreSalt.data());
  const VolumeKey key = DeriveVolumeKey(kGameObbKeyString, kStoreSalt);
  CHECK(std::equal(key.begin(), key.end(), reference.begin()));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

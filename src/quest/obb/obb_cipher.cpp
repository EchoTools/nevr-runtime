#include "quest/obb/obb_cipher.h"

#include <nettle/cbc.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace nevr_quest::obb {

namespace {
constexpr std::size_t kTwofishBlock = 16;
// Sectors decrypted per read of the OBB: 128 KiB.
constexpr std::uint64_t kSectorsPerChunk = 256;
}  // namespace

TwofishCbcPlain::TwofishCbcPlain(const VolumeKey& key) { nettle_twofish_set_key(&ctx_, key.size(), key.data()); }

void TwofishCbcPlain::DecryptSector(std::uint32_t sector, const std::uint8_t* in, std::uint8_t* out) const {
  // dm-crypt "plain": the sector number as a 32-bit little-endian integer, zero-padded to the block.
  std::uint8_t iv[kTwofishBlock] = {};
  iv[0] = static_cast<std::uint8_t>(sector);
  iv[1] = static_cast<std::uint8_t>(sector >> 8);
  iv[2] = static_cast<std::uint8_t>(sector >> 16);
  iv[3] = static_cast<std::uint8_t>(sector >> 24);
  nettle_cbc_decrypt(&ctx_, reinterpret_cast<nettle_cipher_func*>(nettle_twofish_decrypt), kTwofishBlock, iv, kObbSectorBytes, out, in);
}

Status DecryptingSource::ReadAt(std::uint64_t offset, void* dst, std::size_t length) const {
  if (offset > image_bytes_ || length > image_bytes_ - offset) {
    return Status::Fail("source_short_read", "read of " + std::to_string(length) + " bytes at offset " + std::to_string(offset) +
                                                 " is not inside the " + std::to_string(image_bytes_) + "-byte image");
  }
  if (length == 0) return Status::Ok();
  if (image_bytes_ / kObbSectorBytes > 0x100000000ull) {
    return Status::Fail("obb_image_too_large", "the image has more sectors than a 32-bit sector number counts");
  }
  std::vector<std::uint8_t> encrypted(kSectorsPerChunk * kObbSectorBytes);
  std::vector<std::uint8_t> plain(kSectorsPerChunk * kObbSectorBytes);
  std::uint8_t* out = static_cast<std::uint8_t*>(dst);
  std::uint64_t position = offset;
  std::size_t remaining = length;
  while (remaining > 0) {
    const std::uint64_t sector = position / kObbSectorBytes;
    const std::size_t within = static_cast<std::size_t>(position % kObbSectorBytes);
    const std::uint64_t wanted = (within + remaining + kObbSectorBytes - 1) / kObbSectorBytes;
    const std::uint64_t sectors = std::min(wanted, kSectorsPerChunk);
    if (Status s = encrypted_.ReadAt(sector * kObbSectorBytes, encrypted.data(), sectors * kObbSectorBytes); !s.ok) return s;
    for (std::uint64_t i = 0; i < sectors; ++i) {
      cipher_.DecryptSector(static_cast<std::uint32_t>(sector + i), &encrypted[i * kObbSectorBytes], &plain[i * kObbSectorBytes]);
    }
    const std::size_t take = std::min<std::size_t>(remaining, sectors * kObbSectorBytes - within);
    std::memcpy(out, &plain[within], take);
    out += take;
    position += take;
    remaining -= take;
  }
  return Status::Ok();
}

}  // namespace nevr_quest::obb

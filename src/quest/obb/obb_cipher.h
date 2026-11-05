// vold's dm-crypt `twofish` for an OBB: Twofish-128 in CBC mode, one chain per 512-byte sector, the IV of a sector its number
// as a 32-bit little-endian integer zero-padded to 16 bytes (dm-crypt's "plain" IV). Sector 0 is the first 512 bytes of the
// OBB file.
#pragma once

#include <cstdint>

#include <nettle/twofish.h>

#include "quest/obb/byte_source.h"
#include "quest/obb/obb_footer.h"
#include "quest/obb/obb_key.h"

namespace nevr_quest::obb {

class TwofishCbcPlain {
 public:
  explicit TwofishCbcPlain(const VolumeKey& key);
  // Decrypts one sector. `in` and `out` are kObbSectorBytes long and may not overlap.
  void DecryptSector(std::uint32_t sector, const std::uint8_t* in, std::uint8_t* out) const;

 private:
  twofish_ctx ctx_;
};

// The decrypted image of an OBB, read on demand: no sector is decrypted until a read needs it, so the image is never written out.
class DecryptingSource final : public ByteSource {
 public:
  // `encrypted` holds the image from its first byte; `image_bytes` is a whole number of sectors that fits 32-bit sector numbers.
  DecryptingSource(const ByteSource& encrypted, const VolumeKey& key, std::uint64_t image_bytes)
      : encrypted_(encrypted), cipher_(key), image_bytes_(image_bytes) {}
  std::uint64_t Size() const override { return image_bytes_; }
  Status ReadAt(std::uint64_t offset, void* dst, std::size_t length) const override;

 private:
  const ByteSource& encrypted_;
  TwofishCbcPlain cipher_;
  std::uint64_t image_bytes_;
};

}  // namespace nevr_quest::obb

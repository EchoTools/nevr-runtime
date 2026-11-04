#include "quest/obb/obb_key.h"

#include <nettle/pbkdf2.h>

namespace nevr_quest::obb {

VolumeKey DeriveVolumeKey(std::string_view key_string, const std::array<std::uint8_t, kObbSaltBytes>& salt) {
  VolumeKey key{};
  nettle_pbkdf2_hmac_sha1(key_string.size(), reinterpret_cast<const std::uint8_t*>(key_string.data()), kObbKeyRounds, salt.size(),
                          salt.data(), key.size(), key.data());
  return key;
}

}  // namespace nevr_quest::obb

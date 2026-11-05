// The volume key of an OBB: what Android's StorageManager does with the key string the game mounts it with.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "quest/obb/obb_footer.h"

namespace nevr_quest::obb {

// The key string libr15.so passes to AStorageManager_mountObb (CSysFile::Init, call site 0xf85434; docs/adr/0007).
constexpr char kGameObbKeyString[] = "37c70a1635a1ad7a";
// PBKDF2-HMAC-SHA1, 1024 rounds, 16 bytes (Android's StorageManager).
constexpr std::uint32_t kObbKeyRounds = 1024;
constexpr std::size_t kObbKeyBytes = 16;

using VolumeKey = std::array<std::uint8_t, kObbKeyBytes>;

VolumeKey DeriveVolumeKey(std::string_view key_string, const std::array<std::uint8_t, kObbSaltBytes>& salt);

}  // namespace nevr_quest::obb

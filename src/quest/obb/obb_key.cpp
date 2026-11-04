#include "quest/obb/obb_key.h"
namespace nevr_quest::obb {
VolumeKey DeriveVolumeKey(std::string_view, const std::array<std::uint8_t, kObbSaltBytes>&) { return VolumeKey{}; }
}  // namespace nevr_quest::obb

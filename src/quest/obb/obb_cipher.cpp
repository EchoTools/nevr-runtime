#include "quest/obb/obb_cipher.h"
namespace nevr_quest::obb {
TwofishCbcPlain::TwofishCbcPlain(const VolumeKey&) {}
void TwofishCbcPlain::DecryptSector(std::uint32_t, const std::uint8_t*, std::uint8_t*) const {}
Status DecryptingSource::ReadAt(std::uint64_t, void*, std::size_t) const { return Status::Fail("not_implemented", "stub"); }
}  // namespace nevr_quest::obb

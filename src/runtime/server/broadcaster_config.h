#pragma once

#include <cstdint>

// The broadcaster's netconfig mask (CBroadcaster::Initialize, 0x140F7FE50, fourth parameter). Bit 0 is allow_incoming:
// a dedicated lobby accepts incoming connections only when it is set. Bits 1-11 come from the netconfig JSON and are
// never touched here.
namespace nevr_broadcaster_config {

inline constexpr std::uint32_t kAllowIncomingBit = 0x1u;

/// Sets allow_incoming in a netconfig mask. Returns true when the bit was clear and is now set, false when it was
/// already set or when there is no mask (a null mask is left alone).
inline bool ForceAllowIncoming(std::uint32_t* mask) {
  if (mask == nullptr || (*mask & kAllowIncomingBit) != 0) return false;
  *mask |= kAllowIncomingBit;
  return true;
}

}  // namespace nevr_broadcaster_config

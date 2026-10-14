#pragma once

#include <cstdint>

namespace nevr_broadcaster_guard {

// Reject low sentinel addresses before reading the broadcaster's
// listener-table field at +0x5e0.
inline bool TryReadListenerTable(std::uintptr_t outer, std::int64_t& table) {
  if (outer < 0x10000) return false;
  const std::uintptr_t inner = *reinterpret_cast<const std::uintptr_t*>(outer);
  if (inner < 0x10000) return false;
  table = *reinterpret_cast<const std::int64_t*>(inner + 0x5e0);
  return true;
}

}  // namespace nevr_broadcaster_guard

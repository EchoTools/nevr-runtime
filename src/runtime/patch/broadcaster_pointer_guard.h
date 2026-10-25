#pragma once

#include <cstdint>

namespace nevr_broadcaster_guard {

constexpr std::uintptr_t kMaximumUserAddress = 0x7FFFFFFEFFFFULL;

// This rejects sentinel and non-user addresses before each dereference. It does
// not prove the address is mapped: IsReadableMemory uses VirtualQuery, whose
// snapshot can become stale before the read and would run on every dispatch.
inline bool TryReadListenerTable(std::uintptr_t outer, std::int64_t& table) {
  if (outer < 0x10000 || outer > kMaximumUserAddress - sizeof(std::uintptr_t) + 1) return false;
  const std::uintptr_t inner = *reinterpret_cast<const std::uintptr_t*>(outer);
  if (inner < 0x10000 || inner > kMaximumUserAddress - 0x5e0 - sizeof(std::int64_t) + 1) return false;
  table = *reinterpret_cast<const std::int64_t*>(inner + 0x5e0);
  return true;
}

}  // namespace nevr_broadcaster_guard

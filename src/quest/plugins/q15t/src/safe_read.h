// A read that never faults: process_vm_readv on our own pid (an unmapped range is EFAULT, never a signal).
#pragma once

#include <cstddef>
#include <cstdint>

namespace q15t {

bool SafeRead(std::uintptr_t addr, void* out, std::size_t len);

template <typename T>
T Rd(std::uintptr_t addr, bool* ok) {
  T v{};
  if (!SafeRead(addr, &v, sizeof v)) *ok = false;
  return v;
}

}  // namespace q15t

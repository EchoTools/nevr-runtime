#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// One lowercase hex spelling of a byte range, for the log lines that show an expected and an actual prologue.
namespace nevr_hex {

inline constexpr char kDigits[] = "0123456789abcdef";
inline constexpr unsigned kNibbleBits = 4;
inline constexpr unsigned kNibbleMask = 0xF;

// `size` bytes at `bytes`, two digits each, no separator. Total: a null pointer or a zero size gives "". The caller
// passes only memory it has checked is readable.
inline std::string Bytes(const std::uint8_t* bytes, std::size_t size) {
  std::string out;
  if (bytes == nullptr) return out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out += kDigits[bytes[i] >> kNibbleBits];
    out += kDigits[bytes[i] & kNibbleMask];
  }
  return out;
}

}  // namespace nevr_hex

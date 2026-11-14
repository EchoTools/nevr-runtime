// log_text.h: the one way a text from outside becomes a quoted log field (docs/standards/logging.md, Rule 4 and Category M).
// Printable ASCII, with '"' and '\\' written as '?' as every other byte is; at most a caller's bound of bytes. Pure: no
// allocation beyond the result string, and PrintableInto never allocates at all (an out-of-memory line uses it).
#pragma once

#include <cstddef>
#include <string>

namespace nevr_log_text {

inline constexpr unsigned char kFirstPrintable = 0x20;
inline constexpr unsigned char kLastPrintable = 0x7e;

inline char PrintableByte(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return (u < kFirstPrintable || u > kLastPrintable || c == '"' || c == '\\') ? '?' : c;
}

// The text up to its first NUL or `max_bytes`, made printable.
inline std::string Printable(const char* text, std::size_t max_bytes) {
  std::string out;
  if (text == nullptr) return out;
  for (std::size_t n = 0; text[n] != '\0' && n < max_bytes; ++n) out += PrintableByte(text[n]);
  return out;
}

// The same text written into `out` (N bytes, NUL included), for a line where nothing may allocate.
template <std::size_t N>
const char* PrintableInto(const char* text, char (&out)[N]) {
  std::size_t n = 0;
  for (; text != nullptr && text[n] != '\0' && n + 1 < N; ++n) out[n] = PrintableByte(text[n]);
  out[n] = '\0';
  return out;
}

}  // namespace nevr_log_text

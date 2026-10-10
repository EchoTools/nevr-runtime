#pragma once

// The one JSON string escaper for the JSONL logs. Two sinks share the per-character rule:
//   Into()     writes into a caller buffer with no heap use, so the boot log (which runs under the
//              DllMain loader lock) can call it;
//   AppendTo() appends to a std::string, for the runtime log.
// Every control character below 0x20 is escaped, so a line is valid JSON whatever the message holds.

#include <cstddef>
#include <cstdio>
#include <string>

namespace nevr_json_escape {

/// The escaped form of `c` in `out` (not NUL-terminated); returns its length, or 0 when `c` is
/// written as itself.
inline int EscapeChar(char c, char (&out)[7]) {
  switch (c) {
    case '"':  out[0] = '\\'; out[1] = '"';  return 2;
    case '\\': out[0] = '\\'; out[1] = '\\'; return 2;
    case '\n': out[0] = '\\'; out[1] = 'n';  return 2;
    case '\r': out[0] = '\\'; out[1] = 'r';  return 2;
    case '\t': out[0] = '\\'; out[1] = 't';  return 2;
    default: break;
  }
  const unsigned char u = static_cast<unsigned char>(c);
  if (u >= 0x20) return 0;
  static const char kHex[] = "0123456789abcdef";
  out[0] = '\\'; out[1] = 'u'; out[2] = '0'; out[3] = '0';
  out[4] = kHex[u >> 4]; out[5] = kHex[u & 0xF];
  return 6;
}

/// Escapes src[0, src_len) into dst (NUL-terminated, never overruns dst_size, never ends in the
/// middle of an escape sequence). Returns the number of bytes written, excluding the NUL.
inline int Into(const char* src, int src_len, char* dst, int dst_size) {
  if (dst == nullptr || dst_size <= 0) return 0;
  int d = 0;
  for (int i = 0; i < src_len; ++i) {
    char esc[7];
    const int n = EscapeChar(src[i], esc);
    const int need = n == 0 ? 1 : n;
    if (d + need >= dst_size) break;  // leave room for the NUL
    if (n == 0) {
      dst[d++] = src[i];
    } else {
      for (int k = 0; k < n; ++k) dst[d++] = esc[k];
    }
  }
  dst[d] = '\0';
  return d;
}

inline void AppendTo(std::string& out, const char* s, int len) {
  out.reserve(out.size() + static_cast<std::size_t>(len) + 16);
  for (int i = 0; i < len; ++i) {
    char esc[7];
    const int n = EscapeChar(s[i], esc);
    if (n == 0) out += s[i];
    else out.append(esc, static_cast<std::size_t>(n));
  }
}

}  // namespace nevr_json_escape

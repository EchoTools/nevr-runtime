#pragma once

// Text that a plugin, a file name or a config file supplies, made safe to write into one log line (#2215).
//
// Neither existing helper does this. nevr_json_escape (core/json_escape.h) escapes a control byte for the JSON file, so a
// line written to the file stays one record, but it passes every byte at or above 0x20 through unchecked and does nothing
// for the console or the game's logger, where a newline splits the line. nevr_plugin_log (runtime/ext/plugin_log.h) turns a
// control byte into a space, which hides it, and does not look at UTF-8.
//
// SafeText returns ASCII plus well-formed UTF-8 sequences, on one line, and shows every byte it changed: `\n`, `\r`, `\t`
// and `\\` as those two characters, any other control byte, DEL and any byte that is not part of a well-formed UTF-8
// sequence as `\xNN`. The backslash is escaped too, so the output reads back to exactly one input. Header-only, std types
// only, no heap use beyond the returned string: the unit test links it with no host code.

#include <cstddef>
#include <string>
#include <string_view>

namespace nevr_log_text {

/// At most this many input bytes are kept; a longer text ends in "..." so a hostile label cannot crowd the rest of a line
/// out of Log()'s 2048-byte buffer.
inline constexpr std::size_t kMaxInputBytes = 256;

namespace detail {

/// The length of the well-formed UTF-8 sequence that starts at text[i] (RFC 3629: no overlong form, no surrogate, nothing
/// above U+10FFFF), or 0 when none does.
inline std::size_t SequenceLength(std::string_view text, std::size_t i) {
  const auto at = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
  const unsigned char lead = at(i);
  const auto cont = [&](std::size_t k, unsigned char lo = 0x80, unsigned char hi = 0xBF) {
    return k < text.size() && at(k) >= lo && at(k) <= hi;
  };
  if (lead >= 0xC2 && lead <= 0xDF) return cont(i + 1) ? 2 : 0;
  if (lead >= 0xE0 && lead <= 0xEF) {
    const unsigned char lo = lead == 0xE0 ? 0xA0 : 0x80;
    const unsigned char hi = lead == 0xED ? 0x9F : 0xBF;
    return cont(i + 1, lo, hi) && cont(i + 2) ? 3 : 0;
  }
  if (lead >= 0xF0 && lead <= 0xF4) {
    const unsigned char lo = lead == 0xF0 ? 0x90 : 0x80;
    const unsigned char hi = lead == 0xF4 ? 0x8F : 0xBF;
    return cont(i + 1, lo, hi) && cont(i + 2) && cont(i + 3) ? 4 : 0;
  }
  return 0;
}

}  // namespace detail

inline std::string SafeText(std::string_view text) {
  static const char kHex[] = "0123456789abcdef";
  const bool cut = text.size() > kMaxInputBytes;
  if (cut) text = text.substr(0, kMaxInputBytes);
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0x80) {
      const std::size_t n = detail::SequenceLength(text, i);
      if (n != 0) {
        out.append(text.substr(i, n));
        i += n;
        continue;
      }
    }
    ++i;
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c == '\\') out += "\\\\";
    else if (c < 0x20 || c >= 0x7f) {
      out += "\\x";
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    } else {
      out += static_cast<char>(c);
    }
  }
  if (cut) out += "...";
  return out;
}

}  // namespace nevr_log_text

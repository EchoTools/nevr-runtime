#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Text a remote peer sent that did not parse, as a log field (docs/standards/logging.md Rule 4): at most
// kRemoteTextCap bytes of it, inside double quotes, each byte that is not printable ASCII (below 0x20, 0x7f and
// up) and each backslash and double quote written as \xNN. A peer's text therefore cannot end its own field,
// start another (`reason=`, `endpoint_len=`) or start a second line, and spaces stay readable inside the quotes
// Unlike Rule 15's plugin_log.h `Value`, this always quotes and writes a quote or backslash as \xNN, not \".
// The caller logs the full length beside it (`endpoint_len=<n>`,
// `port_len=<n>`), which also says when the cap cut it short.
namespace nevr_remote_text {

constexpr size_t kRemoteTextCap = 64;

// Returns the quoted, escaped value.
inline std::string Escape(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string shown;
  const std::string_view head = text.substr(0, kRemoteTextCap);
  shown.reserve(head.size() + 2);
  shown.push_back('"');
  for (const char c : head) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte < 0x7f && byte != '\\' && byte != '"') {
      shown.push_back(c);
    } else {
      shown += "\\x";
      shown.push_back(kHex[byte >> 4]);
      shown.push_back(kHex[byte & 0x0f]);
    }
  }
  shown.push_back('"');
  return shown;
}

}  // namespace nevr_remote_text

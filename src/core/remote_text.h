#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Text a remote peer sent that did not parse, as a log field (docs/standards/logging.md Rule 4): at most
// kRemoteTextCap bytes of it, each byte that is not printable ASCII (below 0x20, 0x7f and up, and the
// backslash that starts an escape) written as \xNN, so one value is one line and cannot be mistaken for
// another. The caller logs the full length beside it (`len=<n>`), which also says when the cap cut it short.
namespace nevr_remote_text {

constexpr size_t kRemoteTextCap = 64;

inline std::string Escape(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string shown;
  const std::string_view head = text.substr(0, kRemoteTextCap);
  shown.reserve(head.size());
  for (const char c : head) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte < 0x7f && byte != '\\') {
      shown.push_back(c);
    } else {
      shown += "\\x";
      shown.push_back(kHex[byte >> 4]);
      shown.push_back(kHex[byte & 0x0f]);
    }
  }
  return shown;
}

}  // namespace nevr_remote_text

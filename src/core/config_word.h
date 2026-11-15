// config_word.h: how a rejected config.yaml word is named in a log line. A word the file gave is named only when it is short printable
// ASCII (a texture level or a label is a word, not a secret); anything else is described, never repeated. Shared by the Quest's reader
// (quest/sentinel/quest_config.cpp) and the CDN settings reader both runtimes use (cdn/cdn_config.h).
#pragma once

#include <cstddef>
#include <string>

namespace nevr {

// The longest word named in a log line; a longer one is described by its length bound.
inline constexpr std::size_t kMaxNamedConfigWordBytes = 32;
// The bytes of printable ASCII without a space: 0x21 to 0x7e (a byte at or below 0x20, or 0x7f and above, is not).
inline constexpr unsigned char kLastNonPrintableAscii = 0x20;
inline constexpr unsigned char kFirstNonAscii = 0x7f;

inline std::string DescribeRejectedWord(const std::string& text) {
  if (text.empty()) return "(empty)";
  if (text.size() > kMaxNamedConfigWordBytes) return "(longer than " + std::to_string(kMaxNamedConfigWordBytes) + " bytes)";
  for (const char ch : text) {
    const auto u = static_cast<unsigned char>(ch);
    if (u <= kLastNonPrintableAscii || u >= kFirstNonAscii) return "(not printable ASCII without spaces)";
  }
  return text;
}

}  // namespace nevr

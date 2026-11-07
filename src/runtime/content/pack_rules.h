// pack_rules.h: the naming rules a content pack's descriptor is held to, on PC and on Quest (#1424, ADR 0029).
//
// A pack names itself with an id and its files with plain names; both are checked before anything is read or served.
// The PC serving (runtime/content/content_json.h) and the Quest placement (quest/data/pack.cpp) share these, so an id or a
// file name one platform accepts the other accepts too. Header-only, no platform API.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace nevr_content {

// The longest id and the longest plain file name a pack may use.
inline constexpr std::size_t kMaxPackIdLength = 64;
inline constexpr std::size_t kMaxPlainNameLength = 64;

// `size` lowercase hex digits and nothing else.
inline bool IsLowerHex(std::string_view text, std::size_t size) {
  if (text.size() != size) return false;
  return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A character of a plain file name: ASCII letters, digits, '.', '_' and '-'.
inline bool IsPlainChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-';
}

// A pack id: [a-z0-9][a-z0-9._-]{0,63}. It is a folder name under the content folder, so it starts with a letter or a digit
// (never "." or "..") and has no upper case (two ids that differ only in case would be one folder on Windows).
inline bool IsPackId(std::string_view text) {
  if (text.empty() || text.size() > kMaxPackIdLength) return false;
  const char first = text.front();
  if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) return false;
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}

// One plain file name: 1 to 64 plain characters, not starting with a dot (so never "." or "..", and no hidden name).
inline bool IsPlainName(std::string_view text) {
  if (text.empty() || text.size() > kMaxPlainNameLength || text.front() == '.') return false;
  return std::all_of(text.begin(), text.end(), IsPlainChar);
}

}  // namespace nevr_content

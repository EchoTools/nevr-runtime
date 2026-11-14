#pragma once

// The entry grammar and the expected-final-path comparison of the owned-removal guard (core/owned_remove.h). Header-only
// and free of platform calls, so a Linux GTest drives the same code the Win32 and POSIX implementations run.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_owned {

// True when no byte is a C0 control (below 0x20) or DEL (0x7F). An embedded NUL matters most: a C string stops at it, so
// a name checked as a std::string ("..\0/x") would otherwise reach the OS as "..".
inline bool ControlFree(std::string_view s) {
  for (const char c : s) {
    const unsigned char b = static_cast<unsigned char>(c);
    if (b < 0x20 || b == 0x7F) return false;
  }
  return true;
}

// True when `root` may name a folder the guard works under: no control bytes (an embedded NUL in particular).
inline bool ValidRoot(std::string_view root) { return ControlFree(root); }

// True when `entry` is a relative, '/'-separated name a deleter may remove: non-empty, not absolute, with no control byte,
// no '\\' and no ':' anywhere, and every component non-empty and not "." or "..". A trailing '/' makes an empty last
// component, so it is refused too.
inline bool ValidEntry(std::string_view entry) {
  if (entry.empty() || entry.front() == '/') return false;
  if (!ControlFree(entry)) return false;
  for (const char c : entry) {
    if (c == '\\' || c == ':') return false;
  }
  std::size_t start = 0;
  while (true) {
    const std::size_t slash = entry.find('/', start);
    const std::string_view part =
        slash == std::string_view::npos ? entry.substr(start) : entry.substr(start, slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) return true;
    start = slash + 1;
  }
}

// The '/'-separated components of `entry`, in order. Empty components are kept, so a caller that has not checked
// ValidEntry can still see them. An empty entry gives no components.
inline std::vector<std::string> SplitEntry(std::string_view entry) {
  std::vector<std::string> parts;
  if (entry.empty()) return parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t slash = entry.find('/', start);
    if (slash == std::string_view::npos) {
      parts.emplace_back(entry.substr(start));
      return parts;
    }
    parts.emplace_back(entry.substr(start, slash - start));
    start = slash + 1;
  }
}

namespace rules_detail {

constexpr std::wstring_view kLongPathPrefix = L"\\\\?\\";

inline wchar_t FoldAscii(wchar_t c) {
  return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

inline bool EqualFolded(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (FoldAscii(a[i]) != FoldAscii(b[i])) return false;
  }
  return true;
}

inline bool HasLongPathPrefix(std::wstring_view path) {
  return path.size() >= kLongPathPrefix.size() && path.substr(0, kLongPathPrefix.size()) == kLongPathPrefix;
}

}  // namespace rules_detail

// True when `final_path` (what GetFinalPathNameByHandleW reports for the object) is the path expected for `root_final`
// followed by the components `parts`, joined with '\\'. Case-insensitive, ASCII only. The "\\?\" prefix must be on both
// paths or on neither: a mismatch in the prefix is a mismatch. An empty `parts` expects `root_final` itself. An empty
// `root_final`, a "." or ".." or empty part, and a final path of another length all mismatch.
inline bool FinalPathMatches(std::wstring_view final_path, std::wstring_view root_final,
                             const std::vector<std::wstring>& parts) {
  // GetFinalPathNameByHandleW never returns an empty path, so an empty expectation is a caller's bug: refuse it rather than
  // compare two empty strings.
  if (root_final.empty()) return false;
  if (rules_detail::HasLongPathPrefix(final_path) != rules_detail::HasLongPathPrefix(root_final)) return false;
  std::wstring expected(root_final);
  for (const std::wstring& part : parts) {
    if (part.empty() || part == L"." || part == L"..") return false;
    if (expected.empty() || expected.back() != L'\\') expected.push_back(L'\\');
    expected += part;
  }
  return rules_detail::EqualFolded(final_path, expected);
}

}  // namespace nevr_owned

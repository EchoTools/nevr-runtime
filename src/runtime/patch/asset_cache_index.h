#pragma once

// The asset cache's index (patch/asset_cache.cpp): owned.txt, one name per line, the names this cache made and so may remove. Evict
// removes only the names listed here, each by name (nevr_owned::RemoveEntry, core/owned_remove.h), never a name found by listing the
// folder (ORDER.md "The manifest rule"). A name is recorded before the file is created, so a file the cache made is always listed
// when it exists.
//
// Header-only and free of platform calls: the Win32 cache and the Linux host test (src/quest/tests/asset_cache_index_test.cpp) run
// these same rules.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nevr_asset_cache_index {

/// The index's leaf name in the cache folder. Its own write is a literal temp (owned.txt.tmp) and a rename.
inline constexpr char kOwnedIndexName[] = "owned.txt";
/// Largest index the cache reads. A line is at most 80 bytes, so this is far more names than a cache holds.
inline constexpr std::size_t kMaxOwnedIndexBytes = 4u * 1024u * 1024u;

namespace detail {

inline bool IsLowerHex(std::string_view s) {
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// 1 to `max` decimal digits.
inline bool IsDigits(std::string_view s, std::size_t max) {
  if (s.empty() || s.size() > max) return false;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

}  // namespace detail

/// `<64 lowercase hex>.blob`: the only spelling a content-addressed blob has.
inline bool IsBlobLeaf(std::string_view name) {
  constexpr std::string_view kSuffix = ".blob";
  return name.size() == 64 + kSuffix.size() && detail::IsLowerHex(name.substr(0, 64)) && name.substr(64) == kSuffix;
}

/// `<stem>.<pid>.<tick>.<sequence>.tmp` (AssetCache TempPathFor): the stem is a key (a blob's temp) or "index" (index.json's temp),
/// and each number is 1 to 20 decimal digits.
inline bool IsTempLeaf(std::string_view name) {
  constexpr std::string_view kSuffix = ".tmp";
  if (name.size() < kSuffix.size() || name.substr(name.size() - kSuffix.size()) != kSuffix) return false;
  const std::string_view body = name.substr(0, name.size() - kSuffix.size());
  const std::size_t first = body.find('.');
  if (first == std::string_view::npos) return false;
  const std::string_view stem = body.substr(0, first);
  if (!(stem == "index" || (stem.size() == 64 && detail::IsLowerHex(stem)))) return false;
  const std::string_view numbers = body.substr(first + 1);
  const std::size_t second = numbers.find('.');
  if (second == std::string_view::npos) return false;
  const std::size_t third = numbers.find('.', second + 1);
  if (third == std::string_view::npos || numbers.find('.', third + 1) != std::string_view::npos) return false;
  return detail::IsDigits(numbers.substr(0, second), 20) && detail::IsDigits(numbers.substr(second + 1, third - second - 1), 20) &&
         detail::IsDigits(numbers.substr(third + 1), 20);
}

/// A name the index may hold: a blob or a temp of this cache. Nothing else (index.json, notes, another program's file) qualifies.
inline bool IsOwnedLeaf(std::string_view name) { return IsBlobLeaf(name) || IsTempLeaf(name); }

/// Parses the index text: one owned name per line, LF-terminated (a CR before the LF is tolerated). A line that is not an owned name
/// is DROPPED and counted, never trusted: a dropped name is never removed, so the cost is a leaked file, not a wrong delete. A last
/// line with no newline is a torn append and is dropped too (its file was never made). The text is refused only when it is over the
/// size cap. `names` and `dropped` are set on success; the next rewrite of the list writes back only the names kept.
inline bool ParseOwnedIndex(std::string_view text, std::vector<std::string>* names, std::size_t* dropped, std::string* error) {
  if (text.size() > kMaxOwnedIndexBytes) {
    *error = "owned.txt is larger than the cache reads";
    return false;
  }
  std::vector<std::string> out;
  std::size_t lost = 0;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      ++lost;  // torn append
      break;
    }
    std::string_view line = text.substr(start, newline - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) {
      if (IsOwnedLeaf(line)) {
        out.emplace_back(line);
      } else {
        ++lost;
      }
    }
    start = newline + 1;
  }
  *names = std::move(out);
  *dropped = lost;
  return true;
}

/// The index text for `names`: each name and a newline.
inline std::string FormatOwnedIndex(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) {
    out += name;
    out += '\n';
  }
  return out;
}

/// The one-time legacy adoption (an existing cache with no index): a blob is adopted only when its name is a blob name and its content
/// hashes to that name, so it is a blob this cache wrote. A legacy temp is never adopted; it is left in place and logged.
inline bool ShouldAdoptLegacyBlob(std::string_view leaf, std::string_view contentSha256) {
  return !contentSha256.empty() && IsBlobLeaf(leaf) && leaf.substr(0, 64) == contentSha256;
}

/// A temp older than `staleTicks` is a leftover of a killed write. A stamp in the future is never stale.
inline bool IsStaleTemp(std::uint64_t now, std::uint64_t stamp, std::uint64_t staleTicks) {
  return now > stamp && now - stamp > staleTicks;
}

}  // namespace nevr_asset_cache_index

#pragma once

// The run-log index (log/builtin_filter.cpp): runlog-index.txt, one run-log name per line, in the log folder beside the logs. A run
// lists its own log's name, synced, before the file is made, and each rotated archive's name before its rename (a refused name never
// blocks the rotation: the archive is then not listed, a leak, not a wedge). Retention deletes only the names listed here, each by
// name (nevr_owned::RemoveEntry), and never a name found by listing the folder (ORDER.md "The manifest rule"). The grammar below is the
// one core/run_log_path.h and run_log_writer.h Rotate make; any other file in the folder is a stranger: never adopted, never deleted.
//
// Header-only and free of platform calls: the Win32 filter and the Linux host test (src/quest/tests/run_log_index_test.cpp) run the
// same rules.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nevr_run_log_index {

/// The index's leaf name in the log folder. Its own write is a literal temp (runlog-index.txt.tmp) and a rename.
inline constexpr char kRunLogIndexName[] = "runlog-index.txt";
/// Largest index the filter reads: about 200 thousand names of about 40 bytes. Retention removes listed names, so the index shrinks as
/// logs are deleted; past this cap rotation still happens, and the archive is simply not listed (logged once).
inline constexpr std::size_t kMaxRunLogIndexBytes = 8u * 1024u * 1024u;

namespace detail {

inline bool IsDigitsOf(std::string_view s, std::size_t count) {
  if (s.size() != count) return false;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// YYYY-MM-DDTHH-MM-SS.mmm (core/run_log_path.h FormatTimestamp).
inline bool IsStamp(std::string_view s) {
  if (s.size() != 23) return false;
  static constexpr std::string_view kShape = "dddd-dd-ddTdd-dd-dd.ddd";
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (kShape[i] == 'd') {
      if (!IsDigitsOf(s.substr(i, 1), 1)) return false;
    } else if (s[i] != kShape[i]) {
      return false;
    }
  }
  return true;
}

// pid<hex>: the name when the creation time cannot be read. "%lx" makes 1 to 8 lower-case hex digits.
inline bool IsPid(std::string_view s) {
  if (s.size() < 4 || s.substr(0, 3) != "pid") return false;
  const std::string_view hex = s.substr(3);
  if (hex.size() > 8) return false;
  for (const char c : hex) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

inline bool IsCore(std::string_view core) { return IsStamp(core) || IsPid(core); }

}  // namespace detail

/// `nevr-<stamp or pid<hex>>.jsonl`, or the same with a rotated archive number before the suffix: `nevr-<core>.<n>.jsonl` where n is 1
/// to 3 digits.
inline bool IsRunLogLeaf(std::string_view name) {
  constexpr std::string_view kPrefix = "nevr-";
  constexpr std::string_view kSuffix = ".jsonl";
  if (name.size() < kPrefix.size() + kSuffix.size()) return false;
  if (name.substr(0, kPrefix.size()) != kPrefix || name.substr(name.size() - kSuffix.size()) != kSuffix) return false;
  const std::string_view body = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
  if (detail::IsCore(body)) return true;
  const std::size_t dot = body.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return false;
  const std::string_view archive = body.substr(dot + 1);
  return archive.size() >= 1 && archive.size() <= 3 && detail::IsDigitsOf(archive, archive.size()) && detail::IsCore(body.substr(0, dot));
}

/// Parses the index text: one run-log name per line, LF-terminated (a CR before the LF is tolerated). A line that is not a run-log name
/// (a NUL, a torn tail, anything else) is DROPPED and counted: a dropped name is never deleted. The text is refused only when it is over
/// the cap. `names` and `dropped` are set on success; the next rewrite writes back only the names kept, which heals the file.
inline bool ParseRunLogIndex(std::string_view text, std::vector<std::string>* names, std::size_t* dropped, std::string* error) {
  if (text.size() > kMaxRunLogIndexBytes) {
    *error = "runlog-index.txt is larger than the filter reads";
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
      if (IsRunLogLeaf(line)) {
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
inline std::string FormatRunLogIndex(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) {
    out += name;
    out += '\n';
  }
  return out;
}

/// The legacy adoption (a folder with no index) takes a run-log name only when that file is a JSON-lines log: its first byte is '{'.
/// `firstByte` is the file's first byte, or 0 when the file is empty or unreadable (then it is not adopted).
inline bool IsJsonLogStart(char firstByte) { return firstByte == '{'; }

/// The names a one-time legacy adoption takes from a folder listing: exactly the run-log grammar, in listing order. The caller adds the
/// JSON-lines check (IsJsonLogStart) for each file it reads.
inline std::vector<std::string> AdoptableRunLogs(const std::vector<std::string>& listing) {
  std::vector<std::string> adopted;
  for (const std::string& name : listing) {
    if (IsRunLogLeaf(name)) adopted.push_back(name);
  }
  return adopted;
}

}  // namespace nevr_run_log_index

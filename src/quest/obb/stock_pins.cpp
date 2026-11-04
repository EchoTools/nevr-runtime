#include "quest/obb/stock_pins.h"

#include <cerrno>
#include <cstdlib>
#include <map>
#include <set>

namespace nevr_quest::obb {

namespace {

bool IsLowerHex64(std::string_view s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool ParseUnsigned(std::string_view s, std::uint64_t* out) {
  if (s.empty() || s.size() > 19) return false;  // 19 digits always fit
  std::uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  *out = v;
  return true;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// The next space-separated word; `rest` keeps what follows it, trimmed on the left.
std::string_view Word(std::string_view* rest) {
  *rest = Trim(*rest);
  const std::size_t end = rest->find_first_of(" \t");
  const std::string_view word = rest->substr(0, end);
  rest->remove_prefix(end == std::string_view::npos ? rest->size() : end);
  *rest = Trim(*rest);
  return word;
}

bool SafeRelativePath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.back() == '/') return false;
  if (path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

Status Syntax(std::size_t line, const std::string& why) { return Status::Fail("pin_syntax", "line " + std::to_string(line) + ": " + why); }

}  // namespace

Status ParseStockPins(std::string_view text, StockPins* out) {
  StockPins pins;
  bool have_sha = false, have_size = false;
  std::set<std::string> paths;
  std::size_t line_number = 0;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t newline = text.find('\n', at);
    std::string_view line = text.substr(at, newline == std::string_view::npos ? std::string_view::npos : newline - at);
    at = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
    ++line_number;
    line = Trim(line);
    if (line.empty() || line.front() == '#') continue;
    std::string_view rest = line;
    const std::string_view key = Word(&rest);
    if (key == "obb_sha256") {
      const std::string_view digest = Word(&rest);
      if (have_sha) return Syntax(line_number, "obb_sha256 given twice");
      if (!IsLowerHex64(digest) || !rest.empty()) return Syntax(line_number, "obb_sha256 needs one 64-digit lower-case hex digest");
      pins.obb_sha256 = std::string(digest);
      have_sha = true;
    } else if (key == "obb_size") {
      const std::string_view number = Word(&rest);
      if (have_size) return Syntax(line_number, "obb_size given twice");
      if (!ParseUnsigned(number, &pins.obb_size) || !rest.empty()) return Syntax(line_number, "obb_size needs one decimal byte count");
      have_size = true;
    } else if (key == "file") {
      const std::string_view digest = Word(&rest);
      const std::string_view number = Word(&rest);
      PinnedFile f;
      if (!IsLowerHex64(digest)) return Syntax(line_number, "file needs a 64-digit lower-case hex digest first");
      if (!ParseUnsigned(number, &f.size)) return Syntax(line_number, "file needs a decimal byte count after the digest");
      if (rest.empty()) return Syntax(line_number, "file needs a path after the byte count");
      if (!SafeRelativePath(rest)) {
        return Status::Fail("pin_unsafe_path", "line " + std::to_string(line_number) + ": '" + std::string(rest) + "' is not a relative path of plain components");
      }
      f.sha256 = std::string(digest);
      f.path = std::string(rest);
      if (!paths.insert(f.path).second) return Status::Fail("pin_duplicate_file", "line " + std::to_string(line_number) + ": '" + f.path + "' is pinned twice");
      pins.files.push_back(std::move(f));
    } else {
      return Syntax(line_number, "unknown record '" + std::string(key) + "'");
    }
  }
  if (!have_sha || !have_size) return Status::Fail("pin_missing_obb", "the pins need both obb_sha256 and obb_size");
  if (pins.files.empty()) return Status::Fail("pin_no_files", "the pins list no file");
  *out = std::move(pins);
  return Status::Ok();
}

Status CheckObbAgainstPins(const StockPins& pins, const std::string& obb_sha256, std::uint64_t obb_size) {
  if (obb_sha256 != pins.obb_sha256 || obb_size != pins.obb_size) {
    return Status::Fail("pin_obb_mismatch", "this OBB is " + std::to_string(obb_size) + " bytes with SHA-256 " + obb_sha256 + "; the pins accept " +
                                                std::to_string(pins.obb_size) + " bytes with SHA-256 " + pins.obb_sha256);
  }
  return Status::Ok();
}

Status CheckTreeAgainstPins(const StockPins& pins, const std::vector<MeasuredFile>& measured) {
  std::map<std::string, const PinnedFile*> pinned;
  for (const PinnedFile& f : pins.files) pinned[f.path] = &f;
  std::set<std::string> seen;
  for (const MeasuredFile& m : measured) {
    if (!seen.insert(m.path).second) return Status::Fail("pin_duplicate_file", "the tree holds '" + m.path + "' twice");
    const auto it = pinned.find(m.path);
    if (it == pinned.end()) {
      return Status::Fail("pin_unlisted_file", "the tree holds '" + m.path + "' (" + std::to_string(m.size) + " bytes), which the pins do not list");
    }
    if (m.size != it->second->size) {
      return Status::Fail("pin_size_mismatch", "'" + m.path + "' is " + std::to_string(m.size) + " bytes; the pins say " + std::to_string(it->second->size));
    }
    if (m.sha256 != it->second->sha256) {
      return Status::Fail("pin_digest_mismatch", "'" + m.path + "' has SHA-256 " + m.sha256 + "; the pins say " + it->second->sha256);
    }
  }
  for (const PinnedFile& f : pins.files) {
    if (!seen.count(f.path)) return Status::Fail("pin_missing_file", "the pins list '" + f.path + "' and the tree does not hold it");
  }
  return Status::Ok();
}

}  // namespace nevr_quest::obb

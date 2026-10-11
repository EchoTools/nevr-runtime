#pragma once

// The boot log is the crash spool: it is written before the main log exists, and it is never
// deleted. At the main log's first open the lines of THIS run are replayed into it, and once the
// tee has closed the lines it wrote in between are replayed from the byte offset the first replay
// stopped at, so boot and runtime events are one stream (#5). The main log is not ts-ordered
// across that boundary: replayed lines keep their original ts and are appended when replayed.

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_boot_replay {

struct Line {
  std::string ts;
  std::string level;
  std::string msg;
};

/// The lines of `runId` in a nevr-boot.jsonl image, in file order. The file accumulates every run;
/// lines of other runs, lines that are not JSON objects, and lines without a ts or msg (they cannot
/// be placed in the stream) are skipped.
inline std::vector<Line> ParseRun(std::string_view contents, std::string_view runId) {
  std::vector<Line> lines;
  size_t pos = 0;
  while (pos < contents.size()) {
    size_t end = contents.find('\n', pos);
    if (end == std::string_view::npos) end = contents.size();
    const std::string_view text = contents.substr(pos, end - pos);
    pos = end + 1;
    if (text.empty()) continue;
    const nlohmann::json record = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (!record.is_object()) continue;
    const auto run = record.find("run");
    const auto ts = record.find("ts");
    const auto msg = record.find("msg");
    if (run == record.end() || !run->is_string() || run->get<std::string>() != runId) continue;
    if (ts == record.end() || !ts->is_string() || msg == record.end() || !msg->is_string()) continue;
    const auto level = record.find("level");
    lines.push_back({ts->get<std::string>(), level != record.end() && level->is_string() ? level->get<std::string>() : "info",
                     msg->get<std::string>()});
  }
  return lines;
}

/// Where the previous ReadNew stopped: the end of the last complete line it consumed.
struct Cursor {
  bool started = false;
  uint64_t offset = 0;
};

/// The longest tail the first read looks at; the file accumulates every run.
constexpr int64_t kFirstReadTailBytes = 1024 * 1024;

/// The complete lines of `runId` that `path` gained since `cursor`, appended to `out` in file order,
/// and `cursor` moved past them. The first call reads the last kFirstReadTailBytes (dropping the
/// partial line the cut lands in); later calls read from the recorded byte offset to the end, so
/// growth between calls cannot move the window. A trailing line without its newline is another
/// thread's write in progress: it is left for the next call. Returns false, with errno in `*err`,
/// when the file cannot be opened.
inline bool ReadNew(const char* path, std::string_view runId, Cursor& cursor, std::vector<Line>& out, int* err) {
  FILE* in = std::fopen(path, "rb");
  if (in == nullptr) {
    if (err != nullptr) *err = errno;
    return false;
  }
#ifdef _WIN32
  _fseeki64(in, 0, SEEK_END);
  const int64_t size = _ftelli64(in);
#else
  fseeko(in, 0, SEEK_END);
  const int64_t size = ftello(in);
#endif
  int64_t start = 0;
  bool dropPartialFirst = false;
  if (!cursor.started) {
    start = size > kFirstReadTailBytes ? size - kFirstReadTailBytes : 0;
    dropPartialFirst = start > 0;
  } else if (static_cast<int64_t>(cursor.offset) <= size) {
    start = static_cast<int64_t>(cursor.offset);
  }  // a file shorter than the offset was replaced: read it from the top
#ifdef _WIN32
  _fseeki64(in, start, SEEK_SET);
#else
  fseeko(in, start, SEEK_SET);
#endif
  std::string contents(static_cast<size_t>(size > start ? size - start : 0), '\0');
  const size_t got = std::fread(contents.data(), 1, contents.size(), in);
  std::fclose(in);
  contents.resize(got);

  size_t first = 0;
  if (dropPartialFirst) {
    const size_t nl = contents.find('\n');
    first = nl == std::string::npos ? contents.size() : nl + 1;
  }
  const size_t lastNl = contents.rfind('\n');
  const size_t end = lastNl == std::string::npos || lastNl + 1 < first ? first : lastNl + 1;
  for (Line& line : ParseRun(std::string_view(contents).substr(first, end - first), runId)) out.push_back(std::move(line));
  cursor.started = true;
  cursor.offset = static_cast<uint64_t>(start) + end;
  return true;
}

}  // namespace nevr_boot_replay

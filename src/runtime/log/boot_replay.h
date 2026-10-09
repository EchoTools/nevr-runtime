#pragma once

// The boot log is the crash spool: it is written before the main log exists, and it is never
// deleted. At the main log's first open the lines of THIS run are replayed into it, and the lines
// the tee writes after that are replayed once more just before it closes, so boot and runtime
// events are one stream (#5).

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace BootReplay {

struct Line {
  std::string ts;
  std::string level;
  std::string msg;
};

/// The lines of `runId` in a nevr-boot.jsonl image, in file order. The file accumulates every run;
/// lines of other runs, lines that are not JSON objects, and lines without a ts or msg (they cannot
/// be placed in the stream) are skipped. The first `skip` lines of the run are dropped: the lines
/// already replayed by an earlier call.
inline std::vector<Line> ParseRun(std::string_view contents, std::string_view runId, size_t skip = 0) {
  std::vector<Line> lines;
  size_t seen = 0;
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
    if (seen++ < skip) continue;
    const auto level = record.find("level");
    lines.push_back({ts->get<std::string>(), level != record.end() && level->is_string() ? level->get<std::string>() : "info",
                     msg->get<std::string>()});
  }
  return lines;
}

}  // namespace BootReplay

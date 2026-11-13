// The summary line of a Quest crash report (#2129). The handler writes the report under its provisional name, and the summary
// names that name; FinishCrashReport then renames the report beside the minidump. This rule rewrites the summary's `report=`
// field to the final name, so the report names its own file. Header-only, so the host test drives it without the platform.
#pragma once

#include <cstddef>
#include <string>

namespace nevr_quest::crash_summary {

// The report text with the summary line's `report=<provisional>` field changed to `report=<final>`. The text is returned
// unchanged when it has no summary line naming the provisional path, so a second call is a no-op.
inline std::string NameFinalReport(const std::string& text, const std::string& provisional, const std::string& final) {
  constexpr const char kSummaryTag[] = "summary: ";
  const std::size_t summary = text.compare(0, sizeof(kSummaryTag) - 1, kSummaryTag) == 0
                                  ? 0
                                  : text.find(std::string("\n") + kSummaryTag);
  if (summary == std::string::npos) return text;
  const std::size_t lineStart = text[summary] == '\n' ? summary + 1 : summary;
  std::size_t lineEnd = text.find('\n', lineStart);
  if (lineEnd == std::string::npos) lineEnd = text.size();

  const std::string field = std::string("report=") + provisional;
  const std::size_t at = text.find(field, lineStart);
  if (at == std::string::npos || at + field.size() > lineEnd) return text;
  // The field must end at the end of the line or at a space: `report=/a/x` is not `report=/a/x.txt`.
  const std::size_t after = at + field.size();
  if (after != lineEnd && text[after] != ' ') return text;

  return text.substr(0, at) + "report=" + final + text.substr(after);
}

}  // namespace nevr_quest::crash_summary

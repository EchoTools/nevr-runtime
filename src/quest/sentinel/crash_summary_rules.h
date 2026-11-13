// The summary line of a Quest crash report (#2129). The handler writes the report under its provisional name, and the summary
// names that name; FinishCrashReport then renames the report beside the minidump. This rule rewrites the summary's `report=`
// field to the final name, so the report names its own file. Header-only, so the host test drives it without the platform.
//
// FinishCrashReport runs inside the breakpad dump callback, in a crashed process, so the rule allocates nothing and calls no
// stdio: it reads a byte range and writes into a buffer the caller owns.
#pragma once

#include <cstddef>
#include <cstring>

namespace nevr_quest::crash_summary {

constexpr const char kSummaryTag[] = "summary: ";
constexpr std::size_t kSummaryTagLength = sizeof(kSummaryTag) - 1;
constexpr const char kReportField[] = "report=";
constexpr std::size_t kReportFieldLength = sizeof(kReportField) - 1;

namespace detail {

// Whether text[at, size) begins with `report=` followed by the `provisionalLength` bytes of `provisional`.
inline bool FieldAt(const char* text, std::size_t size, std::size_t at, const char* provisional,
                    std::size_t provisionalLength) {
  if (size - at < kReportFieldLength + provisionalLength) return false;
  return std::memcmp(text + at, kReportField, kReportFieldLength) == 0 &&
         std::memcmp(text + at + kReportFieldLength, provisional, provisionalLength) == 0;
}

// The first offset at or after `from` where `report=<provisional>` begins, or `size` when there is none.
inline std::size_t FindField(const char* text, std::size_t size, std::size_t from, const char* provisional,
                             std::size_t provisionalLength) {
  for (std::size_t at = from; at < size; ++at) {
    if (FieldAt(text, size, at, provisional, provisionalLength)) return at;
  }
  return size;
}

// The offset where the summary line begins: 0 when the text opens with it, else just after the first `\nsummary: `; `size`
// when there is no summary line.
inline std::size_t FindSummaryLine(const char* text, std::size_t size) {
  for (std::size_t at = 0; at < size; ++at) {
    const bool lineStart = at == 0 || text[at - 1] == '\n';
    if (lineStart && size - at >= kSummaryTagLength && std::memcmp(text + at, kSummaryTag, kSummaryTagLength) == 0) {
      return at;
    }
  }
  return size;
}

}  // namespace detail

// The report text (`size` bytes, not NUL-terminated) with the summary line's `report=<provisional>` field changed to
// `report=<final>`, written into `out` (`capacity` bytes). `*changed` is false and nothing is written when the text has no
// summary line naming the provisional path, so a second call on the rewritten text changes nothing; the field must end at the
// end of the line or at a space (`report=/a/x` is not `report=/a/x.txt`). Returns false only when the rewritten text does
// not fit in `out`.
inline bool NameFinalReport(const char* text, std::size_t size, const char* provisional, const char* final, char* out,
                            std::size_t capacity, std::size_t* outSize, bool* changed) {
  *changed = false;
  *outSize = 0;
  const std::size_t lineStart = detail::FindSummaryLine(text, size);
  std::size_t lineEnd = lineStart;
  while (lineEnd < size && text[lineEnd] != '\n') ++lineEnd;

  const std::size_t provisionalLength = std::strlen(provisional);
  const std::size_t at = detail::FindField(text, size, lineStart, provisional, provisionalLength);
  const std::size_t after = at + kReportFieldLength + provisionalLength;
  if (at == size || after > lineEnd) return true;
  if (after != lineEnd && text[after] != ' ') return true;

  const std::size_t finalLength = std::strlen(final);
  const std::size_t total = at + kReportFieldLength + finalLength + (size - after);
  if (total > capacity) return false;
  std::memcpy(out, text, at);
  std::memcpy(out + at, kReportField, kReportFieldLength);
  std::memcpy(out + at + kReportFieldLength, final, finalLength);
  std::memcpy(out + at + kReportFieldLength + finalLength, text + after, size - after);
  *outSize = total;
  *changed = true;
  return true;
}

}  // namespace nevr_quest::crash_summary

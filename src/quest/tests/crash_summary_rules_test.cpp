// #2129: after a Quest crash report is renamed beside its minidump, its summary line names the final file, not the provisional one
// the handler wrote. Plain checks; no gtest, no platform. The rule writes into a caller's buffer (it runs in the breakpad
// callback, where nothing may allocate); NameFinalReport below is this test's string view of it.

#include <cstddef>
#include <cstdio>
#include <string>

#include "quest/sentinel/crash_summary_rules.h"

namespace {

int g_failures = 0;

void Expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

// Room for every report below and the final name; the rule's own capacity check is driven by ATooSmallBufferIsRefused.
constexpr std::size_t kOutCapacity = 1024;
constexpr std::size_t kTinyCapacity = 8;
constexpr std::size_t kUnsetSize = 99;

std::string NameFinalReport(const std::string& text, const std::string& provisional, const std::string& final) {
  char out[kOutCapacity];
  std::size_t size = 0;
  bool changed = false;
  const bool fits = nevr_quest::crash_summary::NameFinalReport(text.data(), text.size(), provisional.c_str(), final.c_str(), out,
                                                               sizeof(out), &size, &changed);
  Expect(fits, "the rewritten report fits the test's buffer");
  return changed ? std::string(out, size) : text;
}

const char kProvisional[] = "/d/nevr-crashes/nevr-crash-1793896124-25889.report.txt";
const char kFinal[] = "/d/nevr-crashes/372cb5f7-aa11-4c01-f77775a7-829712bf.report.txt";

std::string Report(const std::string& path) {
  return "nevr crash report v1\nsummary: crash: SIGSEGV libr15.so+0x1a2b3c report=" + path + "\nbuild: 6.16.5-dev.45\nsignal: SIGSEGV\n";
}

void TheSummaryNamesTheFinalFile() {
  const std::string out = NameFinalReport(Report(kProvisional), kProvisional, kFinal);
  Expect(out == Report(kFinal), "summary names the final file and the rest of the report is unchanged");
  Expect(out.find(kProvisional) == std::string::npos, "the provisional name is gone");
}

void ASecondCallIsANoOp() {
  const std::string once = NameFinalReport(Report(kProvisional), kProvisional, kFinal);
  Expect(NameFinalReport(once, kProvisional, kFinal) == once, "a second call changes nothing");
}

void APrefixOfTheProvisionalNameIsNotRewritten() {
  // The field `report=/d/x.txt` is not a match for `report=/d/x.txt.bak` (a longer name), nor for a name it begins.
  const std::string text = "nevr crash report v1\nsummary: crash report=/d/x.txt.bak\nbuild: b\n";
  Expect(NameFinalReport(text, "/d/x.txt", kFinal) == text, "a longer name is left alone");
}

void ATextWithNoSummaryIsUnchanged() {
  const std::string text = "nevr crash report v1\nbuild: b\nsignal: SIGSEGV\n";
  Expect(NameFinalReport(text, kProvisional, kFinal) == text, "no summary line, no change");
}

void TheSummaryLineOnlyIsRewritten() {
  // The provisional path in some other line (a frame, a path note) is not the summary's field.
  const std::string text = "nevr crash report v1\nsummary: crash report=none\nbuild: report=" + std::string(kProvisional) + "\n";
  Expect(NameFinalReport(text, kProvisional, kFinal) == text, "only the summary line changes");
  // The same with the later line's field followed by a space, so only the line bound tells it from the summary's field.
  const std::string spaced = "nevr crash report v1\nsummary: crash report=none\nbuild: report=" + std::string(kProvisional) + " b\n";
  Expect(NameFinalReport(spaced, kProvisional, kFinal) == spaced, "a field in a later line is not the summary's");
}

void ASummaryTagInsideALineIsNotTheSummary() {
  // `summary: ` counts only at the start of a line; the same words later in a line are not the summary line.
  const std::string text = "nevr crash report v1 summary: report=" + std::string(kProvisional) + "\nbuild: b\n";
  Expect(NameFinalReport(text, kProvisional, kFinal) == text, "a summary tag inside a line is not the summary line");
}

void ThePathUnderAnotherFieldIsNotRewritten() {
  // Only the `report=` field is the report's name: the same path under another field, or another path of the same length
  // under `report=`, is left alone.
  const std::string otherField = "nevr crash report v1\nsummary: crash path=" + std::string(kProvisional) + " report=none\n";
  Expect(NameFinalReport(otherField, kProvisional, kFinal) == otherField, "the path under another field is left alone");
  std::string samePath = kProvisional;
  samePath.back() = 'X';
  const std::string sameLength = "nevr crash report v1\nsummary: crash report=" + samePath + " build=b\n";
  Expect(NameFinalReport(sameLength, kProvisional, kFinal) == sameLength, "another path of the same length is left alone");
}

void ATooSmallBufferIsRefused() {
  // The rewrite is refused, not cut short, when the caller's buffer cannot hold it.
  const std::string text = Report(kProvisional);
  char tiny[kTinyCapacity];
  std::size_t size = kUnsetSize;  // set, so the test sees that a refusal writes both outputs
  bool changed = true;
  Expect(!nevr_quest::crash_summary::NameFinalReport(text.data(), text.size(), kProvisional, kFinal, tiny, sizeof(tiny), &size,
                                                     &changed),
         "a buffer too small for the rewrite is refused");
  Expect(!changed && size == 0, "a refused rewrite reports nothing written");
}

void AReportThatEndsInTheFieldIsRewritten() {
  // The field may end the text with no newline after it; the rule reads `size` bytes and never past them.
  const std::string text = "summary: x report=" + std::string(kProvisional);
  Expect(NameFinalReport(text, kProvisional, kFinal) == "summary: x report=" + std::string(kFinal),
         "a summary that is the whole text, ending in the field, is rewritten");
}

}  // namespace

int main() {
  TheSummaryNamesTheFinalFile();
  ASecondCallIsANoOp();
  APrefixOfTheProvisionalNameIsNotRewritten();
  ATextWithNoSummaryIsUnchanged();
  TheSummaryLineOnlyIsRewritten();
  ASummaryTagInsideALineIsNotTheSummary();
  ThePathUnderAnotherFieldIsNotRewritten();
  ATooSmallBufferIsRefused();
  AReportThatEndsInTheFieldIsRewritten();
  if (g_failures != 0) {
    std::printf("crash_summary_rules_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("crash_summary_rules_test: all checks passed\n");
  return 0;
}

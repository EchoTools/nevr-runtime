// #2129: after a Quest crash report is renamed beside its minidump, its summary line names the final file, not the provisional one
// the handler wrote. Plain checks; no gtest, no platform.

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

const char kProvisional[] = "/d/nevr-crashes/nevr-crash-1793896124-25889.report.txt";
const char kFinal[] = "/d/nevr-crashes/372cb5f7-aa11-4c01-f77775a7-829712bf.report.txt";

std::string Report(const std::string& path) {
  return "nevr crash report v1\nsummary: crash: SIGSEGV libr15.so+0x1a2b3c report=" + path + "\nbuild: 6.16.5-dev.45\nsignal: SIGSEGV\n";
}

void TheSummaryNamesTheFinalFile() {
  const std::string out = nevr_quest::crash_summary::NameFinalReport(Report(kProvisional), kProvisional, kFinal);
  Expect(out == Report(kFinal), "summary names the final file and the rest of the report is unchanged");
  Expect(out.find(kProvisional) == std::string::npos, "the provisional name is gone");
}

void ASecondCallIsANoOp() {
  const std::string once = nevr_quest::crash_summary::NameFinalReport(Report(kProvisional), kProvisional, kFinal);
  Expect(nevr_quest::crash_summary::NameFinalReport(once, kProvisional, kFinal) == once, "a second call changes nothing");
}

void APrefixOfTheProvisionalNameIsNotRewritten() {
  // The field `report=/d/x.txt` is not a match for `report=/d/x.txt.bak` (a longer name), nor for a name it begins.
  const std::string text = "nevr crash report v1\nsummary: crash report=/d/x.txt.bak\nbuild: b\n";
  Expect(nevr_quest::crash_summary::NameFinalReport(text, "/d/x.txt", kFinal) == text, "a longer name is left alone");
}

void ATextWithNoSummaryIsUnchanged() {
  const std::string text = "nevr crash report v1\nbuild: b\nsignal: SIGSEGV\n";
  Expect(nevr_quest::crash_summary::NameFinalReport(text, kProvisional, kFinal) == text, "no summary line, no change");
}

void TheSummaryLineOnlyIsRewritten() {
  // The provisional path in some other line (a frame, a path note) is not the summary's field.
  const std::string text = "nevr crash report v1\nsummary: crash report=none\nbuild: report=" + std::string(kProvisional) + "\n";
  Expect(nevr_quest::crash_summary::NameFinalReport(text, kProvisional, kFinal) == text, "only the summary line changes");
}

}  // namespace

int main() {
  TheSummaryNamesTheFinalFile();
  ASecondCallIsANoOp();
  APrefixOfTheProvisionalNameIsNotRewritten();
  ATextWithNoSummaryIsUnchanged();
  TheSummaryLineOnlyIsRewritten();
  if (g_failures != 0) {
    std::printf("crash_summary_rules_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("crash_summary_rules_test: all checks passed\n");
  return 0;
}

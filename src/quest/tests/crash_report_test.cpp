// The shared crash report (src/core/crash_report.h, #1605) on the host: what it prints, what it writes, and in what
// order. The same source builds into the PC runtime and the Quest sentinel; the platform seams are faked here.

#include "core/crash_report.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                                   \
    }                                                                                 \
  } while (0)

bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// One module, libr15.so at 0x7000000000, with one symbol at 0x1a2b00 and a build id.
bool FakeResolve(std::uint64_t address, nevr_crash::Location* out) {
  const std::uint64_t base = 0x7000000000ull;
  if (address < base || address >= base + 0x4000000) return false;
  nevr_crash::CopyText(out->module, sizeof(out->module), "libr15.so");
  nevr_crash::CopyText(out->build_id, sizeof(out->build_id), "0123456789abcdef0123456789abcdef01234567");
  out->module_base = base;
  if (address >= base + 0x1a2b00) {
    nevr_crash::CopyText(out->symbol, sizeof(out->symbol), "CSendEventNode::Enter");
    out->symbol_offset = address - (base + 0x1a2b00);
  }
  return true;
}

nevr_crash::Report SampleReport() {
  static const std::uint64_t kRegs[3] = {0x1111, 0x2222, 0x3333};
  nevr_crash::Report report;
  std::memset(&report, 0, sizeof(report));
  report.signal_name = "SIGSEGV";
  report.signal_number = 11;
  report.fault_code = 1;
  report.fault_address = 8;
  report.has_fault_address = true;
  report.pc = 0x7000000000ull + 0x1a2b3c;
  report.lr = 0x7000000000ull + 0x1a2f10;
  report.has_lr = true;
  const char* names[4] = {"x0", "x1", "x2", "sp"};
  for (std::size_t i = 0; i < 3; ++i) {
    report.registers[i].name = names[i];
    report.registers[i].value = kRegs[i];
  }
  report.registers[3].name = names[3];
  report.registers[3].value = 0x7ffe0000;
  report.register_count = 4;
  report.frames[0] = report.pc;
  report.frames[1] = report.lr;
  report.frames[2] = 0x5555;  // in no module
  report.frame_count = 3;
  nevr_crash::CopyText(report.thread_name, sizeof(report.thread_name), "GameThread");
  report.thread_id = 4242;
  return report;
}

void SummaryNamesSignalModuleSymbolAndPath() {
  const nevr_crash::Report report = SampleReport();
  char line[nevr_crash::kSummaryCapacity];
  nevr_crash::FormatSummary(report, &FakeResolve, "/crashes/a.report.txt", true, line, sizeof(line));
  CHECK(std::strcmp(line, "crash: SIGSEGV libr15.so+0x1a2b3c CSendEventNode::Enter+0x3c "
                          "report=/crashes/a.report.txt") == 0);
}

void SummaryWithoutAModuleKeepsTheAddress() {
  nevr_crash::Report report = SampleReport();
  report.pc = 0x1234;
  char line[nevr_crash::kSummaryCapacity];
  nevr_crash::FormatSummary(report, &FakeResolve, "/crashes/a.report.txt", true, line, sizeof(line));
  CHECK(std::strcmp(line, "crash: SIGSEGV ?+0x1234 ? report=/crashes/a.report.txt") == 0);
}

void SummaryOfAFailedWriteSaysSoAndNamesThePath() {
  const nevr_crash::Report report = SampleReport();
  char line[nevr_crash::kSummaryCapacity];
  nevr_crash::FormatSummary(report, &FakeResolve, "/crashes/a.report.txt", false, line, sizeof(line));
  CHECK(Contains(line, "report=none path=/crashes/a.report.txt write_failed=1"));
  nevr_crash::FormatSummary(report, &FakeResolve, nullptr, false, line, sizeof(line));
  CHECK(Contains(line, "report=none write_failed=1"));
}

void ReportCarriesEveryFactTheIssueNames() {
  nevr_crash::ResetForTest();
  for (int i = 0; i < 60; ++i) {
    char text[64];
    const int n = std::snprintf(text, sizeof(text), "sentinel line %d", i);
    nevr_crash::RecordLogLine(text, static_cast<std::size_t>(n));
  }
  const nevr_crash::Report report = SampleReport();
  static char buffer[nevr_crash::kReportCapacity];
  nevr_crash::FormatReport(report, &FakeResolve, "/crashes/a.report.txt", buffer, sizeof(buffer));
  const std::string text = buffer;
  CHECK(Contains(text, "nevr crash report v1\n"));
  CHECK(Contains(text, "summary: crash: SIGSEGV libr15.so+0x1a2b3c CSendEventNode::Enter+0x3c report=/crashes/a.report.txt\n"));
  CHECK(Contains(text, "signal: SIGSEGV (11) code=1 fault_addr=0x0000000000000008\n"));
  CHECK(Contains(text, "thread: name=GameThread tid=4242\n"));
  CHECK(Contains(text, "pc: libr15.so+0x1a2b3c build_id=0123456789abcdef0123456789abcdef01234567 "
                       "symbol=CSendEventNode::Enter+0x3c\n"));
  CHECK(Contains(text, "lr: libr15.so+0x1a2f10 build_id="));
  CHECK(Contains(text, "  x0=0x0000000000001111 x1=0x0000000000002222 x2=0x0000000000003333\n"));
  CHECK(Contains(text, "  sp=0x000000007ffe0000\n"));
  CHECK(Contains(text, "  #0 libr15.so+0x1a2b3c CSendEventNode::Enter+0x3c\n"));
  CHECK(Contains(text, "  #2 ?+0x5555 ?\n"));
  CHECK(Contains(text, "log (last 50 lines, oldest first):\n"));
  CHECK(Contains(text, "  sentinel line 10\n"));
  CHECK(!Contains(text, "  sentinel line 9\n"));
  CHECK(Contains(text, "  sentinel line 59\n"));
  CHECK(text.find("sentinel line 10") < text.find("sentinel line 59"));
  CHECK(Contains(text, "end of report\n"));
}

void ALogLineCannotStartASecondLine() {
  nevr_crash::ResetForTest();
  const char line[] = "first\nsecond\tthird\x01";
  nevr_crash::RecordLogLine(line, sizeof(line) - 1);
  nevr_crash::Report report = SampleReport();
  static char buffer[nevr_crash::kReportCapacity];
  nevr_crash::FormatReport(report, &FakeResolve, "p", buffer, sizeof(buffer));
  CHECK(Contains(buffer, "  first second third \n"));
}

void ALogLineLongerThanTheSlotIsCut() {
  nevr_crash::ResetForTest();
  const std::string line(nevr_crash::kLogLineBytes + 100, 'x');
  nevr_crash::RecordLogLine(line.data(), line.size());
  nevr_crash::Report report = SampleReport();
  static char buffer[nevr_crash::kReportCapacity];
  nevr_crash::FormatReport(report, &FakeResolve, "p", buffer, sizeof(buffer));
  const std::string text = buffer;
  const std::string cut = "  " + std::string(nevr_crash::kLogLineBytes, 'x') + "\n";
  CHECK(Contains(text, cut.c_str()));
  CHECK(!Contains(text, (cut.substr(0, cut.size() - 1) + "x").c_str()));
}

void AReportThatDoesNotFitEndsOnAMarkerAndStaysTerminated() {
  nevr_crash::ResetForTest();
  const nevr_crash::Report report = SampleReport();
  char buffer[200];
  const std::size_t length = nevr_crash::FormatReport(report, &FakeResolve, "p", buffer, sizeof(buffer));
  CHECK(length == sizeof(buffer) - 1);
  CHECK(buffer[sizeof(buffer) - 1] == '\0' || std::strlen(buffer) == length);
  CHECK(std::strstr(buffer, "[report truncated]") != nullptr);
}

// ---- WriteReport through a fake platform ----

struct Fake {
  std::vector<std::string> events;     // "open <path>", "write <n>", "close", "rename <from> -> <to>"
  std::vector<std::string> contents;   // the bytes of each write
  std::vector<std::string> summaries;
  bool fail_open = false;
  bool fail_rename = false;
};
Fake g_fake;

std::intptr_t FakeOpen(const char* path) {
  g_fake.events.push_back(std::string("open ") + path);
  return g_fake.fail_open ? -1 : 7;
}
bool FakeWrite(std::intptr_t handle, const char* data, std::size_t size) {
  CHECK(handle == 7);
  g_fake.events.push_back("write");
  g_fake.contents.emplace_back(data, size);
  return true;
}
void FakeClose(std::intptr_t) { g_fake.events.push_back("close"); }
bool FakeRename(const char* from, const char* to) {
  g_fake.events.push_back(std::string("rename ") + from + " -> " + to);
  return !g_fake.fail_rename;
}
void FakeLog(const char* line) { g_fake.summaries.emplace_back(line); }
void FakeDescribe(const nevr_crash::CrashInfo& info, nevr_crash::Report* report) {
  report->signal_name = "SIGSEGV";
  report->has_fault_address = true;
  report->has_lr = true;
  nevr_crash::CopyText(report->thread_name, sizeof(report->thread_name), "T");
  CHECK(info.context == reinterpret_cast<void*>(0x99));
}
bool FakePath(const nevr_crash::CrashInfo&, char* buffer, std::size_t capacity) {
  nevr_crash::CopyText(buffer, capacity, "/crashes/s.report.txt");
  return true;
}

nevr_crash::Platform FakePlatform() {
  return {&FakeResolve, &FakeDescribe, &FakePath, &FakeOpen, &FakeWrite, &FakeClose, &FakeRename, &FakeLog};
}

nevr_crash::CrashInfo SampleInfo() {
  nevr_crash::CrashInfo info = {};
  info.code = 11;
  info.fault_address = 8;
  info.pc = 0x7000000000ull + 0x1a2b3c;
  info.lr = 0x7000000000ull + 0x1a2f10;
  info.context = reinterpret_cast<void*>(0x99);
  info.thread_id = 4242;
  info.fault_code = 1;
  return info;
}

void AReportIsWrittenUnderATempNameAndRenamedLast() {
  nevr_crash::ResetForTest();
  g_fake = Fake();
  const nevr_crash::Platform platform = FakePlatform();
  nevr_crash::SetPlatform(&platform);
  CHECK(!nevr_crash::ReportWritten());
  CHECK(nevr_crash::WriteReport(SampleInfo()));
  CHECK(nevr_crash::ReportWritten());
  CHECK(std::strcmp(nevr_crash::LastReportPath(), "/crashes/s.report.txt") == 0);
  // Raw pass, then the symbolized pass over it: each writes the temp file and renames it, and nothing names the final
  // path before a rename.
  const std::vector<std::string> expected = {
      "open /crashes/s.report.txt.tmp", "write", "close", "rename /crashes/s.report.txt.tmp -> /crashes/s.report.txt",
      "open /crashes/s.report.txt.tmp", "write", "close", "rename /crashes/s.report.txt.tmp -> /crashes/s.report.txt"};
  CHECK(g_fake.events == expected);
  CHECK(g_fake.contents.size() == 2);
  CHECK(!Contains(g_fake.contents[0], "libr15.so+0x1a2b3c"));  // the first pass has no symbol lookup
  CHECK(Contains(g_fake.contents[0], "pc: ?+0x70001a2b3c"));
  CHECK(Contains(g_fake.contents[1], "pc: libr15.so+0x1a2b3c"));
  CHECK(g_fake.summaries.size() == 1);
  CHECK(g_fake.summaries[0] == "crash: SIGSEGV libr15.so+0x1a2b3c CSendEventNode::Enter+0x3c report=/crashes/s.report.txt");
}

void AFailedOpenIsLoggedAsAFailedWrite() {
  nevr_crash::ResetForTest();
  g_fake = Fake();
  g_fake.fail_open = true;
  const nevr_crash::Platform platform = FakePlatform();
  nevr_crash::SetPlatform(&platform);
  CHECK(!nevr_crash::WriteReport(SampleInfo()));
  CHECK(!nevr_crash::ReportWritten());
  CHECK(g_fake.summaries.size() == 1);
  CHECK(Contains(g_fake.summaries[0], "report=none path=/crashes/s.report.txt write_failed=1"));
  for (const std::string& event : g_fake.events) CHECK(event.rfind("rename", 0) != 0);
}

void AFailedRenameIsNotAWrittenReport() {
  nevr_crash::ResetForTest();
  g_fake = Fake();
  g_fake.fail_rename = true;
  const nevr_crash::Platform platform = FakePlatform();
  nevr_crash::SetPlatform(&platform);
  CHECK(!nevr_crash::WriteReport(SampleInfo()));
  CHECK(Contains(g_fake.summaries.at(0), "write_failed=1"));
}

void NoPlatformIsNoReport() {
  nevr_crash::ResetForTest();
  g_fake = Fake();
  CHECK(!nevr_crash::WriteReport(SampleInfo()));
  CHECK(g_fake.events.empty());
  CHECK(g_fake.summaries.empty());
}

void HexTextAndCopyTextBoundTheirOutput() {
  char hex[7];
  const std::uint8_t bytes[4] = {0xde, 0xad, 0xbe, 0xef};
  nevr_crash::HexText(hex, sizeof(hex), bytes, 4);
  CHECK(std::strcmp(hex, "deadbe") == 0);
  char text[4];
  nevr_crash::CopyText(text, sizeof(text), "abcdef");
  CHECK(std::strcmp(text, "abc") == 0);
}

}  // namespace

int main() {
  SummaryNamesSignalModuleSymbolAndPath();
  SummaryWithoutAModuleKeepsTheAddress();
  SummaryOfAFailedWriteSaysSoAndNamesThePath();
  ReportCarriesEveryFactTheIssueNames();
  ALogLineCannotStartASecondLine();
  ALogLineLongerThanTheSlotIsCut();
  AReportThatDoesNotFitEndsOnAMarkerAndStaysTerminated();
  AReportIsWrittenUnderATempNameAndRenamedLast();
  AFailedOpenIsLoggedAsAFailedWrite();
  AFailedRenameIsNotAWrittenReport();
  NoPlatformIsNoReport();
  HexTextAndCopyTextBoundTheirOutput();
  if (g_failures != 0) {
    std::fprintf(stderr, "crash_report_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("crash_report_test: all checks passed");
  return 0;
}

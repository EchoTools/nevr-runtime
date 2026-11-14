#include "quest/sentinel/crash_report_quest.h"

#include "core/crash_report.h"
#include "core/update/names.h"
#include "quest/sentinel/crash_summary_rules.h"
#include "quest/sentinel/elf_build_id.h"
#include "quest/sentinel/sentinel.h"
#include "quest/sentinel/sentinel_log.h"

#include <android/log.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define NEVR_TAG "NEVR-Sentinel"

// Everything here runs in a fault handler on a compromised process: raw syscalls and fixed storage only. The
// platform table is constant-initialized, so the sentinel's one ELF constructor stays its only load-time
// initializer (tools/check_quest_static_init.sh).

namespace sentinel {

namespace {

constexpr std::size_t kRegisterCount = 34;  // x0..x30, sp, pc, pstate
constexpr std::uint64_t kAddressMask = 0x0000ffffffffffffull;  // the 48 address bits of a return address
constexpr std::uintptr_t kPageSize = 4096;
constexpr mode_t kReportFileMode = 0644;  // the report and its sibling while the summary is named: rw-r--r--

const char* const kRegisterNames[kRegisterCount] = {
    "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10", "x11",
    "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp",  "pc",  "pstate"};

const char* SignalName(std::int64_t signal_number) {
  switch (signal_number) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    default: return "unknown";
  }
}

// Signals whose si_addr is the faulting address when the kernel raised them (si_code > 0).
bool CarriesFaultAddress(std::int64_t signal_number) {
  return signal_number == SIGSEGV || signal_number == SIGBUS || signal_number == SIGILL ||
         signal_number == SIGFPE || signal_number == SIGTRAP;
}

// A page the kernel says is mapped. mincore is a plain syscall; it reads nothing and takes no lock.
bool Readable(const void* address, std::size_t length) {
  if (address == nullptr || length == 0) return false;
  const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(address) & ~(kPageSize - 1);
  const std::uintptr_t last = reinterpret_cast<std::uintptr_t>(address) + length - 1;
  if (last < first) return false;
  for (std::uintptr_t page = first; page <= last; page += kPageSize) {
    unsigned char vec = 0;
    if (::mincore(reinterpret_cast<void*>(page), kPageSize, &vec) != 0) return false;
  }
  return true;
}

std::uint64_t StripTag(std::uint64_t address) { return address & kAddressMask; }

// pc, lr and the frame-pointer chain from x29. A frame record is {previous fp, return address}; the chain has to
// climb and stay in mapped memory, or the walk stops.
void BuildFrames(const std::uint64_t* registers, nevr_crash::Report* report) {
  std::size_t count = 0;
  auto push = [&](std::uint64_t address) {
    if (address == 0 || count >= nevr_crash::kMaxFrames) return;
    if (count > 0 && report->frames[count - 1] == address) return;
    report->frames[count++] = address;
  };
  push(report->pc);
  push(StripTag(report->lr));
  std::uint64_t fp = registers[29];
  const std::uint64_t sp = registers[31];
  for (std::size_t depth = 0; depth < nevr_crash::kMaxFrames && count < nevr_crash::kMaxFrames; ++depth) {
    if (fp == 0 || (fp & 7) != 0 || fp < sp || !Readable(reinterpret_cast<const void*>(fp), 16)) break;
    const auto* record = reinterpret_cast<const std::uint64_t*>(fp);
    const std::uint64_t next = record[0];
    push(StripTag(record[1]));
    if (next <= fp) break;
    fp = next;
  }
  report->frame_count = count;
}

bool ResolveAddress(std::uint64_t address, nevr_crash::Location* out) {
  Dl_info info;
  std::memset(&info, 0, sizeof(info));
  if (::dladdr(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), &info) == 0 ||
      info.dli_fname == nullptr || info.dli_fbase == nullptr) {
    return false;
  }
  const char* name = std::strrchr(info.dli_fname, '/');
  nevr_crash::CopyText(out->module, sizeof(out->module), name != nullptr ? name + 1 : info.dli_fname);
  out->module_base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.dli_fbase));
  if (info.dli_sname != nullptr) {
    nevr_crash::CopyText(out->symbol, sizeof(out->symbol), info.dli_sname);
    out->symbol_offset = address - static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.dli_saddr));
  }
  std::uint8_t id[32];
  std::size_t id_length = 0;
  if (ReadElfBuildId(info.dli_fbase, &Readable, id, sizeof(id), &id_length)) {
    nevr_crash::HexText(out->build_id, sizeof(out->build_id), id, id_length);
  }
  return true;
}

std::intptr_t OpenFile(const char* path) {
  return ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kReportFileMode);
}

bool WriteFile(std::intptr_t handle, const char* data, std::size_t size) {
  int err = 0;
  return WriteAll(static_cast<int>(handle), data, size, &err, ::write);
}

void CloseFile(std::intptr_t handle) { ::close(static_cast<int>(handle)); }

void LogSummary(const char* line) { __android_log_write(ANDROID_LOG_FATAL, NEVR_TAG, line); }

void AppendDec(char* buffer, std::size_t capacity, std::size_t* length, std::uint64_t value) {
  char digits[24];
  std::size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (count > 0 && *length + 1 < capacity) buffer[(*length)++] = digits[--count];
  buffer[*length] = '\0';
}

void AppendText(char* buffer, std::size_t capacity, std::size_t* length, const char* text) {
  while (*text != '\0' && *length + 1 < capacity) buffer[(*length)++] = *text++;
  buffer[*length] = '\0';
}

// The minidump path the breakpad callback announced; empty until then. Written only by FinishCrashReport.
char g_dump_path[kReportPathBytes];

bool RenameFile(const char* from, const char* to) { return ::rename(from, to) == 0; }

// `<dump path without .dmp>` + the report suffix: the report beside its minidump, under the same stem.
bool BesideMinidump(const char* dump_path, char* buffer, std::size_t capacity) {
  std::size_t length = std::strlen(dump_path);
  const char* dmp = nevr_crash::kMinidumpSuffix;
  const std::size_t dmp_length = std::strlen(dmp);
  if (length >= dmp_length && std::strcmp(dump_path + length - dmp_length, dmp) == 0) length -= dmp_length;
  const char* suffix = nevr_crash::kReportSuffix;
  if (length + std::strlen(suffix) + 1 > capacity) return false;
  std::memcpy(buffer, dump_path, length);
  std::memcpy(buffer + length, suffix, std::strlen(suffix) + 1);
  return true;
}

// The minidump's stem when the callback announced it; otherwise a provisional one,
// `<dump dir>/nevr-crash-<unix seconds>-<tid>`, that FinishCrashReport renames to the minidump's once it is known.
bool ReportPath(const nevr_crash::CrashInfo& info, char* buffer, std::size_t capacity) {
  if (g_dump_path[0] != '\0') return BesideMinidump(g_dump_path, buffer, capacity);
  const char* dir = DumpDir();
  if (dir == nullptr || dir[0] == '\0' || capacity == 0) return false;
  std::size_t length = 0;
  buffer[0] = '\0';
  AppendText(buffer, capacity, &length, dir);
  AppendText(buffer, capacity, &length, "/nevr-crash-");
  AppendDec(buffer, capacity, &length, static_cast<std::uint64_t>(::time(nullptr)));
  AppendText(buffer, capacity, &length, "-");
  AppendDec(buffer, capacity, &length, info.thread_id);
  AppendText(buffer, capacity, &length, nevr_crash::kReportSuffix);
  return true;
}

void Describe(const nevr_crash::CrashInfo& info, nevr_crash::Report* report) {
  report->signal_name = SignalName(info.code);
  report->has_lr = true;
  report->has_fault_address = CarriesFaultAddress(info.code) && info.fault_code > 0;
  // The thread name is read for the thread running this code, which is the faulting one when a signal handler calls.
  if (info.thread_id == static_cast<std::uint64_t>(::syscall(SYS_gettid))) {
    char name[17];
    std::memset(name, 0, sizeof(name));
    ::prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0);
    nevr_crash::CopyText(report->thread_name, sizeof(report->thread_name), name);
  }
#if defined(__aarch64__)
  const auto* uc = static_cast<const ucontext_t*>(info.context);
  if (uc != nullptr) {
    std::uint64_t registers[kRegisterCount];
    for (std::size_t i = 0; i < 31; ++i) registers[i] = uc->uc_mcontext.regs[i];
    registers[31] = uc->uc_mcontext.sp;
    registers[32] = uc->uc_mcontext.pc;
    registers[33] = uc->uc_mcontext.pstate;
    for (std::size_t i = 0; i < kRegisterCount; ++i) {
      report->registers[i].name = kRegisterNames[i];
      report->registers[i].value = registers[i];
    }
    report->register_count = kRegisterCount;
    BuildFrames(registers, report);
  }
#endif
}

// The report beside the minidump names the provisional file in its summary (#2129). Rewrites that field to the final name:
// read the whole report, change the summary line (crash_summary_rules.h), write it under a sibling name and rename it over the
// report. Best effort: on a failure the report stays whole under its final name, with the provisional path in its summary,
// and the log says so.
//
// This runs inside the breakpad dump callback, in a crashed process: raw syscalls (open, read, write, close, rename, unlink)
// and the fixed buffers below only, no stdio and no allocation. The buffers are zero-initialized, so they add no load-time
// initializer. One thread at a time reaches them: OnSignal (crash_first_hook.cpp) runs the dump action, and so this callback,
// only on the thread that took g_claim.
constexpr char kNamingSuffix[] = ".naming";
// The report as read, one byte more than a report can be, so a larger file is seen and left alone.
char g_namingInput[nevr_crash::kReportCapacity + 1];
// The rewritten report: the final path replaces the provisional one, and the final path is shorter than kReportPathBytes.
char g_namingOutput[nevr_crash::kReportCapacity + kReportPathBytes];
char g_namingPath[kReportPathBytes + sizeof(kNamingSuffix)];

bool NameReportInSummary(const char* path, const char* provisional, const char* final) {
  const std::size_t pathLength = std::strlen(path);
  if (pathLength + sizeof(kNamingSuffix) > sizeof(g_namingPath)) return false;
  const int in = ::open(path, O_RDONLY | O_CLOEXEC);
  if (in < 0) return false;
  std::size_t size = 0;
  bool readFailed = false;
  while (size < sizeof(g_namingInput)) {
    const ssize_t got = ::read(in, g_namingInput + size, sizeof(g_namingInput) - size);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      readFailed = got < 0;
      break;
    }
    size += static_cast<std::size_t>(got);
  }
  ::close(in);
  if (readFailed || size > nevr_crash::kReportCapacity) return false;

  std::size_t namedSize = 0;
  bool changed = false;
  if (!nevr_quest::crash_summary::NameFinalReport(g_namingInput, size, provisional, final, g_namingOutput,
                                                  sizeof(g_namingOutput), &namedSize, &changed)) {
    return false;
  }
  if (!changed) return true;
  std::memcpy(g_namingPath, path, pathLength);
  std::memcpy(g_namingPath + pathLength, kNamingSuffix, sizeof(kNamingSuffix));
  const int out = ::open(g_namingPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kReportFileMode);
  if (out < 0) return false;
  int err = 0;
  const bool wrote = WriteAll(out, g_namingOutput, namedSize, &err, ::write);
  const bool closed = ::close(out) == 0;
  if (!wrote || !closed || ::rename(g_namingPath, path) != 0) {
    ::unlink(g_namingPath);
    return false;
  }
  return true;
}

// ---- the self-update's crash marker (docs/adr/0024-pc-self-update.md, section 6) ----

// update/pending.json and update/crashed under the nevr root, built once by ArmUpdateCrashMarker. Zero-initialized, so the file
// keeps no load-time initializer; g_markerArmed turns on only after both paths are whole.
char g_pendingPath[kReportPathBytes];
char g_crashedPath[kReportPathBytes];
std::atomic<bool> g_markerArmed{false};

constexpr char kSeparator[] = "/";
constexpr mode_t kMarkerFileMode = 0600;        // the empty marker, readable by its owner only (the app's own folder)
constexpr std::size_t kMarkerLineBytes = 1024;  // the arm-time logcat line when the record is unavailable: a fixed text, a reason and a number

// `root`, "/" unless root ends with one, "update", "/", `leaf`, into `out` (`capacity` bytes, NUL included). False, and `out`
// untouched, when it does not fit.
bool JoinUpdatePath(char* out, std::size_t capacity, const char* root, const char* leaf) {
  const std::size_t rootLength = std::strlen(root);
  const bool rootEndsWithSeparator = rootLength > 0 && root[rootLength - 1] == kSeparator[0];
  const std::size_t total = rootLength + (rootEndsWithSeparator ? 0 : std::strlen(kSeparator)) +
                            std::strlen(nevr_update_names::kUpdateDir) + std::strlen(kSeparator) + std::strlen(leaf);
  if (total + 1 > capacity) return false;
  std::size_t length = 0;
  out[0] = '\0';
  AppendText(out, capacity, &length, root);
  if (!rootEndsWithSeparator) AppendText(out, capacity, &length, kSeparator);
  AppendText(out, capacity, &length, nevr_update_names::kUpdateDir);
  AppendText(out, capacity, &length, kSeparator);
  AppendText(out, capacity, &length, leaf);
  return true;
}

// ---- the marker's record in the run's log ----
//
// The PC host writes a `crash_marker` record into the process's main JSONL log (docs/standards/logging.md, the NEVR.HOST row:
// `outcome`, `win32_error`, `path`, `thread`). This writes the same record into the sentinel's log, `nevr-<timestamp>.jsonl`
// (sentinel_log.h); `errno` is the POSIX name of `win32_error`. ArmUpdateCrashMarker opens that file for appending once
// (OpenCrashRecordFile) and builds the part of the record that never changes, once per outcome; the crash path adds the thread, the
// time and the errno in a stack buffer and appends the line with write(2). The crash path allocates nothing, takes no lock and
// calls nothing of liblog or of the sentinel log. The line is built by hand because nlohmann::json allocates; the host test parses
// what it produces.
//
// The keys are the log's own header keys (`ts`, `run`, `level`, `tag`, `event`) and the PC record's fields (`outcome`, `path`,
// `thread`), written in key order as every record of that file is, plus `errno` on a failed marker:
//   {"event":"crash_marker","level":"info","outcome":"written","path":"<crashed>","run":"<run>","tag":"NEVR.HOST","thread":<tid>,"ts":"<UTC>"}
//   {"errno":<n>,"event":"crash_marker","level":"warn","outcome":"failed","path":"<crashed>","run":"<run>","tag":"NEVR.HOST","thread":<tid>,"ts":"<UTC>"}
// Info for a written marker, Warning for one that failed.

constexpr std::size_t kRecordPrefixBytes = 1024;  // each pre-built prefix with its NUL: fixed text, the escaped path and run id
// What the crash path adds to a prefix, NUL included: `{"errno":` and up to 20 digits, the thread's up to 20 digits, `,"ts":"`, the
// 24-character stamp and `"}` and the newline.
constexpr std::size_t kRecordVariableBytes = 128;
constexpr std::size_t kRecordLineBytes = kRecordPrefixBytes + kRecordVariableBytes;

constexpr char kRecordOpenObject[] = "{";
constexpr char kRecordContinueObject[] = ",";
constexpr char kRecordErrnoOpening[] = "{\"errno\":";
constexpr char kRecordEventAndLevel[] = "\"event\":\"crash_marker\",\"level\":\"";
constexpr char kRecordOutcomeKey[] = "\",\"outcome\":\"";
constexpr char kRecordPathKey[] = "\",\"path\":\"";
constexpr char kRecordRunKey[] = "\",\"run\":\"";
constexpr char kRecordTagAndThreadKey[] = "\",\"tag\":\"NEVR.HOST\",\"thread\":";
constexpr char kRecordTimestampKey[] = ",\"ts\":\"";
constexpr char kRecordClose[] = "\"}\n";
constexpr char kLevelInfoText[] = "info";
constexpr char kLevelWarnText[] = "warn";
constexpr char kOutcomeWrittenText[] = "written";
constexpr char kOutcomeFailedText[] = "failed";

// The fixed part of each record, up to and including `"thread":`: zero-initialized, built by ArmUpdateCrashMarker. The failed one
// starts with a comma, after the `{"errno":<n>` the crash path writes in front of it.
char g_recordWritten[kRecordPrefixBytes];
char g_recordFailed[kRecordPrefixBytes];
// The descriptor ArmUpdateCrashMarker opened with OpenCrashRecordFile, or -1 when it could not: then the crash path writes no record.
std::atomic<int> g_recordFd{-1};

constexpr std::uint64_t kDecimalBase = 10;
constexpr std::size_t kMaxDecimalDigits = 20;  // UINT64_MAX is 20 decimal digits
constexpr unsigned char kFirstPrintableAscii = 0x20;  // a byte below it is a control character, which JSON writes escaped
constexpr unsigned kNibbleBits = 4;
constexpr unsigned kNibbleMask = 0x0f;
constexpr char kHexDigits[] = "0123456789abcdef";

// The appenders keep `buffer` NUL-terminated and return false, leaving a partial text, when the text does not fit. They are plain
// loops over bytes: the crash path calls them.
bool AppendChar(char* buffer, std::size_t capacity, std::size_t* length, char c) {
  if (*length + 1 >= capacity) return false;  // the character and the NUL
  buffer[(*length)++] = c;
  buffer[*length] = '\0';
  return true;
}

bool AppendRaw(char* buffer, std::size_t capacity, std::size_t* length, const char* text) {
  for (; *text != '\0'; ++text) {
    if (!AppendChar(buffer, capacity, length, *text)) return false;
  }
  return true;
}

// `text` as the inside of a JSON string: the quote, the backslash and the control characters escaped, every other byte as it is (a
// path of the app's folders is UTF-8).
bool AppendJsonEscaped(char* buffer, std::size_t capacity, std::size_t* length, const char* text) {
  for (; *text != '\0'; ++text) {
    const unsigned char c = static_cast<unsigned char>(*text);
    bool ok = true;
    if (c == '"' || c == '\\') {
      ok = AppendChar(buffer, capacity, length, '\\') && AppendChar(buffer, capacity, length, *text);
    } else if (c < kFirstPrintableAscii) {
      ok = AppendRaw(buffer, capacity, length, "\\u00") && AppendChar(buffer, capacity, length, kHexDigits[c >> kNibbleBits]) &&
           AppendChar(buffer, capacity, length, kHexDigits[c & kNibbleMask]);
    } else {
      ok = AppendChar(buffer, capacity, length, *text);
    }
    if (!ok) return false;
  }
  return true;
}

// `value` in decimal, at least `minDigits` wide (zero-padded on the left).
bool AppendNumber(char* buffer, std::size_t capacity, std::size_t* length, std::uint64_t value, std::size_t minDigits) {
  char digits[kMaxDecimalDigits];
  std::size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % kDecimalBase);
    value /= kDecimalBase;
  } while (value != 0);
  while (count < minDigits && count < kMaxDecimalDigits) digits[count++] = '0';
  while (count > 0) {
    if (!AppendChar(buffer, capacity, length, digits[--count])) return false;
  }
  return true;
}

// The proleptic Gregorian calendar by integer arithmetic (the days-to-civil-date algorithm of Howard Hinnant's date library), so the
// crash path needs neither gmtime nor strftime. The year is counted from 1 March, so the leap day is the last day of a year.
constexpr std::int64_t kCivilEpochShiftDays = 719468;  // days from 0000-03-01 to 1970-01-01
constexpr std::int64_t kYearsPerEra = 400;             // an era is the Gregorian cycle
constexpr std::int64_t kDaysPerEra = 146097;           // 400 years: 97 leap days
constexpr std::int64_t kDaysPerCentury = 36524;        // 100 years: 24 leap days
constexpr std::int64_t kDaysPerFourYears = 1460;       // 4 * 365, before the leap day
constexpr std::int64_t kDaysPerYear = 365;
constexpr std::int64_t kLeapCycleYears = 4;
constexpr std::int64_t kCenturyYears = 100;
constexpr std::int64_t kMonthStepFactor = 5;           // (5 * day-of-year + 2) / 153 is the month counted from March
constexpr std::int64_t kMonthStepOffset = 2;
constexpr std::int64_t kMonthStepDays = 153;           // the days of March to July
constexpr std::int64_t kMarch = 3;
constexpr std::int64_t kFebruary = 2;
constexpr std::int64_t kMonthsPerYear = 12;
constexpr std::int64_t kSecondsPerDay = 86400;
constexpr std::int64_t kSecondsPerHour = 3600;
constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kNanosPerMilli = 1000000;
constexpr std::size_t kYearDigits = 4;
constexpr std::size_t kFieldDigits = 2;
constexpr std::size_t kMillisDigits = 3;

struct CivilDate {
  std::int64_t year;
  std::int64_t month;  // 1 to 12
  std::int64_t day;    // 1 to 31
};

// The date of `days` since 1970-01-01, for days >= 0.
CivilDate CivilFromDays(std::int64_t days) {
  const std::int64_t shifted = days + kCivilEpochShiftDays;
  const std::int64_t era = shifted / kDaysPerEra;
  const std::int64_t dayOfEra = shifted - era * kDaysPerEra;
  const std::int64_t yearOfEra =
      (dayOfEra - dayOfEra / kDaysPerFourYears + dayOfEra / kDaysPerCentury - dayOfEra / (kDaysPerEra - 1)) / kDaysPerYear;
  std::int64_t year = yearOfEra + era * kYearsPerEra;
  const std::int64_t dayOfYear = dayOfEra - (kDaysPerYear * yearOfEra + yearOfEra / kLeapCycleYears - yearOfEra / kCenturyYears);
  const std::int64_t monthFromMarch = (kMonthStepFactor * dayOfYear + kMonthStepOffset) / kMonthStepDays;
  const std::int64_t day = dayOfYear - (kMonthStepDays * monthFromMarch + kMonthStepOffset) / kMonthStepFactor + 1;
  std::int64_t month = monthFromMarch + kMarch;
  if (month > kMonthsPerYear) month -= kMonthsPerYear;
  if (month <= kFebruary) ++year;
  return CivilDate{year, month, day};
}

// `YYYY-MM-DDTHH:MM:SS.mmmZ`, UTC, as every record of the sentinel log writes its `ts`. A time before the epoch reads as the epoch.
bool AppendTimestamp(char* buffer, std::size_t capacity, std::size_t* length, const timespec& now) {
  const std::int64_t seconds = now.tv_sec > 0 ? static_cast<std::int64_t>(now.tv_sec) : 0;
  const std::int64_t millis = now.tv_nsec > 0 ? static_cast<std::int64_t>(now.tv_nsec) / kNanosPerMilli : 0;
  const std::int64_t secondOfDay = seconds % kSecondsPerDay;
  const CivilDate date = CivilFromDays(seconds / kSecondsPerDay);
  return AppendNumber(buffer, capacity, length, static_cast<std::uint64_t>(date.year), kYearDigits) &&
         AppendChar(buffer, capacity, length, '-') &&
         AppendNumber(buffer, capacity, length, static_cast<std::uint64_t>(date.month), kFieldDigits) &&
         AppendChar(buffer, capacity, length, '-') &&
         AppendNumber(buffer, capacity, length, static_cast<std::uint64_t>(date.day), kFieldDigits) &&
         AppendChar(buffer, capacity, length, 'T') &&
         AppendNumber(buffer, capacity, length, static_cast<std::uint64_t>(secondOfDay / kSecondsPerHour), kFieldDigits) &&
         AppendChar(buffer, capacity, length, ':') &&
         AppendNumber(buffer, capacity, length,
                      static_cast<std::uint64_t>((secondOfDay % kSecondsPerHour) / kSecondsPerMinute), kFieldDigits) &&
         AppendChar(buffer, capacity, length, ':') &&
         AppendNumber(buffer, capacity, length, static_cast<std::uint64_t>(secondOfDay % kSecondsPerMinute), kFieldDigits) &&
         AppendChar(buffer, capacity, length, '.') &&
         AppendNumber(buffer, capacity, length, static_cast<std::uint64_t>(millis), kMillisDigits) &&
         AppendChar(buffer, capacity, length, 'Z');
}

// One record line, newline included, into `out` (`capacity` bytes): `{"errno":<failure>` first for a failed marker, then the
// pre-built prefix, the thread, the stamp and the close. Returns the line's length, or 0 when it does not fit. The one place the
// record's text is put together on the crash path.
std::size_t FormatMarkerRecord(char* out, std::size_t capacity, CrashMarkerOutcome outcome, int failure, std::uint64_t thread,
                               const timespec& now) {
  const bool written = outcome == CrashMarkerOutcome::kWritten;
  std::size_t length = 0;
  out[0] = '\0';
  const bool errnoDone = written || (AppendRaw(out, capacity, &length, kRecordErrnoOpening) &&
                                     AppendNumber(out, capacity, &length, static_cast<std::uint64_t>(failure), 0));
  const bool done = errnoDone && AppendRaw(out, capacity, &length, written ? g_recordWritten : g_recordFailed) &&
                    AppendNumber(out, capacity, &length, thread, 0) && AppendRaw(out, capacity, &length, kRecordTimestampKey) &&
                    AppendTimestamp(out, capacity, &length, now) && AppendRaw(out, capacity, &length, kRecordClose);
  return done ? length : 0;
}

// The crash path's part: one record line appended to the descriptor opened at arm time, in one write(2) (WriteAll retries EINTR and
// a short write). No descriptor, no record, and no second try to open one: a failed write cannot be reported from here, the
// marker file is written either way.
void WriteMarkerRecord(CrashMarkerOutcome outcome, int failure) noexcept {
  const int fd = g_recordFd.load(std::memory_order_acquire);
  if (fd < 0) return;
  timespec now{};
  ::clock_gettime(CLOCK_REALTIME, &now);
  char line[kRecordLineBytes];
  const std::size_t size =
      FormatMarkerRecord(line, sizeof(line), outcome, failure, static_cast<std::uint64_t>(::syscall(SYS_gettid)), now);
  if (size == 0) return;
  int err = 0;
  WriteAll(fd, line, size, &err, ::write);
}

// `<opening>"event":"crash_marker","level":"<level>","outcome":"<outcome>","path":"<path>","run":"<run>","tag":"NEVR.HOST","thread":`
// into `out`. False when it does not fit.
bool BuildRecordPrefix(char* out, std::size_t capacity, const char* opening, const char* level, const char* outcome,
                       const char* path, const char* run) {
  std::size_t length = 0;
  out[0] = '\0';
  return AppendRaw(out, capacity, &length, opening) && AppendRaw(out, capacity, &length, kRecordEventAndLevel) &&
         AppendRaw(out, capacity, &length, level) && AppendRaw(out, capacity, &length, kRecordOutcomeKey) &&
         AppendRaw(out, capacity, &length, outcome) && AppendRaw(out, capacity, &length, kRecordPathKey) &&
         AppendJsonEscaped(out, capacity, &length, path) && AppendRaw(out, capacity, &length, kRecordRunKey) &&
         AppendJsonEscaped(out, capacity, &length, run) && AppendRaw(out, capacity, &length, kRecordTagAndThreadKey);
}

// Arm time: the two prefixes, then the descriptor. Returns the descriptor, or -1 with `*reason` (a word) and `*err` (errno, 0 when
// the failure has none) set. The prefixes come first so a record that cannot fit creates no file.
int OpenMarkerRecord(const char** reason, int* err) {
  *err = 0;
  const char* run = LogRunId();
  if (!BuildRecordPrefix(g_recordWritten, sizeof(g_recordWritten), kRecordOpenObject, kLevelInfoText, kOutcomeWrittenText,
                         g_crashedPath, run) ||
      !BuildRecordPrefix(g_recordFailed, sizeof(g_recordFailed), kRecordContinueObject, kLevelWarnText, kOutcomeFailedText,
                         g_crashedPath, run)) {
    *reason = "record_too_long";
    return -1;
  }
  const int fd = OpenCrashRecordFile();
  if (fd < 0) {
    *err = errno;
    *reason = "log_not_opened";
  }
  return fd;
}

// The marker still works without its record; this says the record will be missing, once, at arm time (liblog is fine here).
void WarnRecordUnavailable(const char* reason, int err) {
  char line[kMarkerLineBytes];
  std::size_t length = 0;
  line[0] = '\0';
  AppendText(line, sizeof(line), &length, "[NEVR.HOST] crash_marker record unavailable reason=");
  AppendText(line, sizeof(line), &length, reason);
  AppendText(line, sizeof(line), &length, " errno=");
  AppendDec(line, sizeof(line), &length, static_cast<std::uint64_t>(err));
  AppendText(line, sizeof(line), &length, " (update/crashed is still written; this run's log gets no crash_marker record)");
  __android_log_write(ANDROID_LOG_WARN, NEVR_TAG, line);
}

const nevr_crash::Platform kPlatform = {&ResolveAddress, &Describe,  &ReportPath,  &OpenFile,
                                        &WriteFile,      &CloseFile, &RenameFile, &LogSummary};

}  // namespace

void RegisterCrashReportPlatform() { nevr_crash::SetPlatform(&kPlatform); }

bool FinishCrashReport(const char* dump_path, char* report_path, unsigned long capacity) {
  if (report_path == nullptr || capacity == 0) return false;
  report_path[0] = '\0';
  if (dump_path == nullptr || dump_path[0] == '\0') return false;
  char target[kReportPathBytes];
  if (!BesideMinidump(dump_path, target, sizeof(target))) return false;

  if (nevr_crash::ReportWritten()) {
    // The provisional path is where the handler wrote it. Move it beside the minidump; rename is one atomic syscall.
    const char* provisional = nevr_crash::LastReportPath();
    if (std::strcmp(provisional, target) != 0 && ::rename(provisional, target) != 0) {
      nevr_crash::CopyText(report_path, capacity, provisional);  // still a whole report, under the provisional stem
      return true;
    }
    // The renamed report's summary names the file it is now (#2129); a failed rewrite leaves the provisional path in it.
    if (std::strcmp(provisional, target) != 0 && !NameReportInSummary(target, provisional, target)) {
      __android_log_print(ANDROID_LOG_WARN, NEVR_TAG, "crash: report summary still names the provisional file path=%s", target);
    }
    nevr_crash::CopyText(report_path, capacity, target);
    return true;
  }

  nevr_crash::CopyText(g_dump_path, sizeof(g_dump_path), dump_path);
  nevr_crash::CrashInfo info = {};
  info.thread_id = static_cast<std::uint64_t>(::syscall(SYS_gettid));
  if (!nevr_crash::WriteReport(info)) return false;
  nevr_crash::CopyText(report_path, capacity, nevr_crash::LastReportPath());
  return true;
}

bool ArmUpdateCrashMarker(const char* nevr_root) {
  g_markerArmed.store(false, std::memory_order_release);
  // An earlier arm's descriptor is closed first, so arming again leaks nothing and the crash path never writes to a stale one.
  const int previousFd = g_recordFd.exchange(-1, std::memory_order_acq_rel);
  if (previousFd >= 0) ::close(previousFd);
  if (nevr_root == nullptr || nevr_root[0] == '\0') {
    __android_log_print(ANDROID_LOG_WARN, NEVR_TAG, "[NEVR.UPDATE] crash_marker not armed reason=no_root");
    return false;
  }
  if (!JoinUpdatePath(g_pendingPath, sizeof(g_pendingPath), nevr_root, nevr_update_names::kPendingJson) ||
      !JoinUpdatePath(g_crashedPath, sizeof(g_crashedPath), nevr_root, nevr_update_names::kCrashedFile)) {
    __android_log_print(ANDROID_LOG_WARN, NEVR_TAG, "[NEVR.UPDATE] crash_marker not armed reason=path_too_long");
    return false;
  }
  // The record's descriptor is open before the marker is armed, so a crash that finds the marker armed finds the descriptor too.
  const char* recordReason = "";
  int recordErrno = 0;
  const int recordFd = OpenMarkerRecord(&recordReason, &recordErrno);
  if (recordFd < 0) WarnRecordUnavailable(recordReason, recordErrno);
  g_recordFd.store(recordFd, std::memory_order_release);
  g_markerArmed.store(true, std::memory_order_release);
  __android_log_print(ANDROID_LOG_INFO, NEVR_TAG, "[NEVR.UPDATE] crash_marker armed record=%s",
                      recordFd >= 0 ? "open" : "unavailable");
  return true;
}

CrashMarkerOutcome MarkUpdateCrashed() noexcept {
  const int savedErrno = errno;
  CrashMarkerOutcome outcome = CrashMarkerOutcome::kNotArmed;
  int failure = 0;
  if (!g_markerArmed.load(std::memory_order_acquire)) {
    outcome = CrashMarkerOutcome::kNotArmed;
  } else if (::access(g_pendingPath, F_OK) != 0) {
    // No pending release: nothing to roll back, so no file and no record. Any access() failure reads as "not there", as the
    // host's GetFileAttributesW does (ADR 0024, section 6).
    outcome = CrashMarkerOutcome::kNoPending;
  } else {
    // O_NOFOLLOW: a crashed that is a link is refused (ELOOP), never followed to a file of another place.
    const int handle = ::open(g_crashedPath, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, kMarkerFileMode);
    if (handle >= 0) {
      ::close(handle);
      outcome = CrashMarkerOutcome::kWritten;
    } else {
      failure = errno;
      outcome = CrashMarkerOutcome::kFailed;
    }
  }
  if (outcome == CrashMarkerOutcome::kWritten || outcome == CrashMarkerOutcome::kFailed) WriteMarkerRecord(outcome, failure);
  errno = savedErrno;
  return outcome;
}

}  // namespace sentinel

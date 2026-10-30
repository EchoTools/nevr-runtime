#pragma once

// The crash report both platforms write (#1605). Echo VR is one codebase built for echovr.exe (PC) and
// libr15.so (Quest): what a report says, how it reads and the one summary line are written once, here. Each
// platform supplies only what it alone can know through a small seam: where an address falls (Platform::resolve),
// how to open and write a file, and where the summary line goes.
//
// Crash-safe by construction (N70 on PC, async-signal-safety on Quest): nothing here allocates, locks, throws,
// calls the logger or enumerates modules. The report is formatted into a static buffer, the last log lines live in
// a static ring, and the file is written with the platform's raw open and write. The shared source includes no
// platform header, so the same bytes build under mingw, the NDK and a host compiler.
//
// The one entry point a crash handler calls is WriteReport(CrashInfo). The handler (Quest signal handler, PC vectored
// handler) hands over the raw facts and a pointer to its own context (ucontext_t* or EXCEPTION_POINTERS*); the
// platform registered with SetPlatform reads the registers, the backtrace and the thread name out of that context.
//
// A report is text, one fact per line, readable without a tool:
//
//   nevr crash report v1
//   summary: crash: SIGSEGV libr15.so+0x1a2b3c CSendEventNode::Enter+0x14 report=<path>
//   signal: SIGSEGV (11) code=1 fault_addr=0x0000000000000008
//   thread: name=GameThread tid=4242
//   pc: libr15.so+0x1a2b3c build_id=<hex> symbol=CSendEventNode::Enter+0x14
//   lr: libr15.so+0x1a2f10 build_id=<hex> symbol=...
//   registers:
//     x0=0x... x1=0x... x2=0x...
//   backtrace:
//     #0 libr15.so+0x1a2b3c CSendEventNode::Enter+0x14
//   log (last 50 lines, oldest first):
//     ...
//   end of report

#include <cstddef>
#include <cstdint>

namespace nevr_crash {

// The one spelling of the names a crash leaves in the crash folder. The report is `<stem>` + kReportSuffix; the minidump
// beside it is `<stem>` + kMinidumpSuffix. The sender (core/crash_send.h) looks for the first and never reads the second.
inline constexpr const char* kReportSuffix = ".report.txt";
inline constexpr const char* kMinidumpSuffix = ".dmp";

constexpr std::size_t kLogLineCount = 50;
constexpr std::size_t kLogLineBytes = 320;
constexpr std::size_t kMaxFrames = 32;
constexpr std::size_t kMaxRegisters = 40;
constexpr std::size_t kReportCapacity = 32 * 1024;
constexpr std::size_t kSummaryCapacity = 512;
constexpr std::size_t kModuleNameBytes = 96;
constexpr std::size_t kBuildIdBytes = 72;
constexpr std::size_t kSymbolBytes = 160;

// Where an address falls. `module` is empty when the address belongs to no module the platform knows.
struct Location {
  char module[kModuleNameBytes];
  char build_id[kBuildIdBytes];   // lower-case hex; empty when the module carries none
  char symbol[kSymbolBytes];      // nearest exported symbol at or below the address; empty when none
  std::uint64_t module_base;
  std::uint64_t symbol_offset;    // address - symbol address
};

// Fills `out` (zeroed by the caller) and returns true when `address` falls inside a known module. Runs in the
// crash path: no allocation, no locks the faulting thread could hold.
using ResolveFn = bool (*)(std::uint64_t address, Location* out);

struct Register {
  const char* name;
  std::uint64_t value;
};

constexpr std::size_t kThreadNameBytes = 48;

// What a crash handler hands to WriteReport. The first six fields are the fixed contract between the handlers and the
// report; fields after them are additions.
struct CrashInfo {
  std::int64_t code;             // the signal number (Quest) or the exception code (PC)
  std::uint64_t fault_address;   // the address the access faulted on; 0 when the fault has none
  std::uint64_t pc;
  std::uint64_t lr;              // the link register; 0 where the architecture has none (x86-64)
  void* context;                 // ucontext_t* on Quest, EXCEPTION_POINTERS* on PC; may be null
  std::uint64_t thread_id;
  std::int64_t fault_code = 0;   // si_code on Quest; 0 on PC
};

// The facts the report prints, as the platform's describe() fills them from a CrashInfo and its context.
struct Report {
  const char* signal_name;
  std::int64_t signal_number;
  bool signal_number_hex;        // print signal_number as hex (an exception code) instead of decimal
  std::int64_t fault_code;
  std::uint64_t fault_address;
  bool has_fault_address;
  std::uint64_t pc;
  std::uint64_t lr;
  bool has_lr;
  Register registers[kMaxRegisters];
  std::size_t register_count;
  std::uint64_t frames[kMaxFrames];   // return addresses, innermost first; frames[0] is normally pc
  std::size_t frame_count;
  char thread_name[kThreadNameBytes];  // empty when unknown
  std::uint64_t thread_id;
};

// The platform seams, registered once with SetPlatform. Without open_file, write_file and close_file a report is
// logged but not written (the summary line says so).
struct Platform {
  ResolveFn resolve;
  // Fills the platform's part of `report` (signal_name, signal_number_hex, has_fault_address, has_lr, registers, frames,
  // thread_name) from `info`, whose context pointer is the platform's own. The common fields are already set.
  void (*describe)(const CrashInfo& info, Report* report);
  // Writes the report's final path (`<stem>` + the upload suffix) into `buffer`; returns false when there is none.
  bool (*report_path)(const CrashInfo& info, char* buffer, std::size_t capacity);
  // Opens `path` for writing, creating or truncating it; returns -1 on failure.
  std::intptr_t (*open_file)(const char* path);
  bool (*write_file)(std::intptr_t handle, const char* data, std::size_t size);
  void (*close_file)(std::intptr_t handle);
  // Renames `from` to `to`, replacing `to`; atomic where the platform can be. The report is written under a temporary
  // name and renamed last, so nothing that scans the crash directory ever sees half a report.
  bool (*rename_file)(const char* from, const char* to);
  // Receives the one summary line, NUL-terminated, without a newline.
  void (*log_summary)(const char* line);
};

// Registers the platform. Call once at init, before any crash can reach WriteReport. `platform` must outlive the process.
void SetPlatform(const Platform* platform);

// Writes the report for `info` and sends the one summary line to the platform's log. One report at a time: a call made
// while another runs (a fault inside the handler) returns false at once, as does a call before SetPlatform. Returns
// true when the whole file was written.
bool WriteReport(const CrashInfo& info) noexcept;

// True once a report has been written in this process, so a second handler (the breakpad dump callback after the
// signal handler) does not write the same crash twice.
bool ReportWritten();

// The path of the report most recently written whole, or an empty string when none was.
const char* LastReportPath();

// Records one log line into the static ring of the last kLogLineCount lines. Thread-safe and allocation-free, so
// it is callable from the log path of any thread. A control character becomes a space and a line past
// kLogLineBytes is cut. `length` is the byte count of `line`, which need not be NUL-terminated.
void RecordLogLine(const char* line, std::size_t length);

// `crash: <signal> <module>+<offset> <symbol> report=<path>`. When the report could not be written the last field
// is `report=none path=<path> write_failed=1`. Returns the length written, excluding the NUL.
std::size_t FormatSummary(const Report& report, ResolveFn resolve, const char* report_path, bool written,
                          char* buffer, std::size_t capacity);

// The whole report text, NUL-terminated and cut with a marker when `capacity` is too small. Returns its length.
std::size_t FormatReport(const Report& report, ResolveFn resolve, const char* report_path, char* buffer,
                         std::size_t capacity);

// Copies `source` into `destination` (capacity bytes, NUL-terminated, cut when it does not fit). For the
// platform resolvers that fill a Location.
void CopyText(char* destination, std::size_t capacity, const char* source);

// Writes the lower-case hex of `bytes` into `destination`, NUL-terminated, cut when it does not fit.
void HexText(char* destination, std::size_t capacity, const std::uint8_t* bytes, std::size_t count);

// Test seam: empties the log ring and forgets the registered platform and the written flag.
void ResetForTest();

}  // namespace nevr_crash

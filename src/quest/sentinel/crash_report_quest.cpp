#include "quest/sentinel/crash_report_quest.h"

#include "core/crash_report.h"
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
  return ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
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
char g_dump_path[640];

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

const nevr_crash::Platform kPlatform = {&ResolveAddress, &Describe,  &ReportPath,  &OpenFile,
                                        &WriteFile,      &CloseFile, &RenameFile, &LogSummary};

}  // namespace

void RegisterCrashReportPlatform() { nevr_crash::SetPlatform(&kPlatform); }

bool FinishCrashReport(const char* dump_path, char* report_path, unsigned long capacity) {
  if (report_path == nullptr || capacity == 0) return false;
  report_path[0] = '\0';
  if (dump_path == nullptr || dump_path[0] == '\0') return false;
  char target[640];
  if (!BesideMinidump(dump_path, target, sizeof(target))) return false;

  if (nevr_crash::ReportWritten()) {
    // The provisional path is where the handler wrote it. Move it beside the minidump; rename is one atomic syscall.
    const char* provisional = nevr_crash::LastReportPath();
    if (std::strcmp(provisional, target) != 0 && ::rename(provisional, target) != 0) {
      nevr_crash::CopyText(report_path, capacity, provisional);  // still a whole report, under the provisional stem
      return true;
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

}  // namespace sentinel

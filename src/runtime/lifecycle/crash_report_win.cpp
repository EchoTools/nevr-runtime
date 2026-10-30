#include "runtime/lifecycle/crash_report_win.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string.h>

#include "core/crash_report.h"
#include "core/logging.h"
#include "runtime/compat/crash_upload.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/crash_recovery_sites.h"
#include "runtime/lifecycle/crash_report_pe.h"
#include "runtime/lifecycle/readable_memory.h"

namespace nevr_crash_win {

namespace {

constexpr std::size_t kPathCapacity = MAX_PATH + 32;
constexpr int kStackScanQwords = 512;

char g_stem[kPathCapacity];  // `<crashes dir>\nevr-crash-<run id>`; empty when there is no directory
ModuleLookupFn g_lookup = nullptr;
volatile LONG g_reportCount = 0;

struct NamedCode {
  DWORD code;
  const char* name;
};

const NamedCode kCodes[] = {
    {EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION"},
    {EXCEPTION_ILLEGAL_INSTRUCTION, "EXCEPTION_ILLEGAL_INSTRUCTION"},
    {EXCEPTION_STACK_OVERFLOW, "EXCEPTION_STACK_OVERFLOW"},
    {EXCEPTION_INT_DIVIDE_BY_ZERO, "EXCEPTION_INT_DIVIDE_BY_ZERO"},
    {static_cast<DWORD>(STATUS_STACK_BUFFER_OVERRUN), "STATUS_STACK_BUFFER_OVERRUN"},
    {0x20474343, "C++ exception from a NEVR module"},
};

const char* CodeName(DWORD code) {
  for (const NamedCode& entry : kCodes) {
    if (entry.code == code) return entry.name;
  }
  return "unknown exception";
}

bool Readable(const void* address, std::size_t length) {
  return nevr_crash_recovery::IsReadableMemory(address, length);
}

bool ResolveAddress(std::uint64_t address, nevr_crash::Location* out) {
  ModuleSpan span{};
  if (g_lookup == nullptr || !g_lookup(address, &span)) return false;
  nevr_crash::CopyText(out->module, sizeof(out->module), span.name);
  out->module_base = span.base;
  const auto* image = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(span.base));
  nevr_crash_pe::ImageIdentity identity{};
  if (nevr_crash_pe::ReadIdentity(image, &Readable, &identity)) {
    nevr_crash_pe::FormatIdentity(identity, out->build_id, sizeof(out->build_id));
  }
  const std::uint64_t rva = address - span.base;
  std::uint64_t offset = 0;
  if (rva <= 0xffffffffull && nevr_crash_pe::NearestExport(image, &Readable, static_cast<std::uint32_t>(rva),
                                                           out->symbol, sizeof(out->symbol), &offset)) {
    out->symbol_offset = offset;
    return true;
  }
  // The game has no exports. The server-mode attribution table names the functions the null-dereference family hits;
  // an exact function start is the only match it is allowed to claim.
  if (_stricmp(span.name, "echovr.exe") == 0) {
    if (const char* site = nevr_crash_recovery::LookupKnownNullDerefSite(static_cast<std::int64_t>(rva))) {
      nevr_crash::CopyText(out->symbol, sizeof(out->symbol), site);
      out->symbol_offset = 0;
    }
  }
  return true;
}

void Describe(const nevr_crash::CrashInfo& info, nevr_crash::Report* report) {
  report->signal_name = CodeName(static_cast<DWORD>(info.code));
  report->signal_number_hex = true;
  report->has_lr = false;
  if (info.thread_id == GameLoopThreadId()) {
    nevr_crash::CopyText(report->thread_name, sizeof(report->thread_name), "game-loop");
  }

  const auto* pointers = static_cast<const EXCEPTION_POINTERS*>(info.context);
  if (pointers == nullptr || pointers->ContextRecord == nullptr) return;
  const CONTEXT* ctx = pointers->ContextRecord;
  if (pointers->ExceptionRecord != nullptr) {
    const EXCEPTION_RECORD* rec = pointers->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
      report->has_fault_address = true;
      report->fault_address = static_cast<std::uint64_t>(rec->ExceptionInformation[1]);
      report->fault_code = static_cast<std::int64_t>(rec->ExceptionInformation[0]);  // 0 read, 1 write, 8 execute
    }
  }

  const nevr_crash::Register registers[] = {
      {"rax", ctx->Rax}, {"rbx", ctx->Rbx}, {"rcx", ctx->Rcx}, {"rdx", ctx->Rdx}, {"rsi", ctx->Rsi},
      {"rdi", ctx->Rdi}, {"rbp", ctx->Rbp}, {"rsp", ctx->Rsp}, {"r8", ctx->R8},   {"r9", ctx->R9},
      {"r10", ctx->R10}, {"r11", ctx->R11}, {"r12", ctx->R12}, {"r13", ctx->R13}, {"r14", ctx->R14},
      {"r15", ctx->R15}, {"rip", ctx->Rip}};
  report->register_count = sizeof(registers) / sizeof(registers[0]);
  std::memcpy(report->registers, registers, sizeof(registers));

  // x64 keeps no frame chain, so the backtrace is what the existing dump does: the words on the stack that point into a
  // module the snapshot knows (return addresses), innermost first, after the faulting rip.
  std::size_t count = 0;
  report->frames[count++] = ctx->Rip;
  if (static_cast<DWORD>(info.code) == EXCEPTION_STACK_OVERFLOW) {
    report->frame_count = count;  // the stack is the one resource that ran out; do not scan it
    return;
  }
  const auto* sp = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(ctx->Rsp));
  for (int i = 0; i < kStackScanQwords && count < nevr_crash::kMaxFrames; ++i) {
    if (!nevr_crash_recovery::IsReadableMemory(sp + i, 8)) break;
    ModuleSpan span{};
    if (g_lookup != nullptr && g_lookup(sp[i], &span)) report->frames[count++] = sp[i];
  }
  report->frame_count = count;
}

// `<stem>-<n>` + the report suffix: one file per report, never appended to.
bool ReportPath(const nevr_crash::CrashInfo&, char* buffer, std::size_t capacity) {
  if (g_stem[0] == '\0') return false;
  const LONG number = InterlockedIncrement(&g_reportCount);
  const int length = std::snprintf(buffer, capacity, "%s-%ld%s", g_stem, number, nevr_crash_upload::kReportSuffix);
  return length > 0 && static_cast<std::size_t>(length) < capacity;
}

bool RenameFile(const char* from, const char* to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0; }

std::intptr_t OpenFile(const char* path) {
  const HANDLE handle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  return handle == INVALID_HANDLE_VALUE ? -1 : reinterpret_cast<std::intptr_t>(handle);
}

bool WriteFileAll(std::intptr_t handle, const char* data, std::size_t size) {
  const HANDLE file = reinterpret_cast<HANDLE>(handle);
  std::size_t done = 0;
  while (done < size) {
    DWORD written = 0;
    if (!WriteFile(file, data + done, static_cast<DWORD>(size - done), &written, nullptr) || written == 0) {
      return false;
    }
    done += written;
  }
  return FlushFileBuffers(file) != 0;
}

void CloseFile(std::intptr_t handle) { CloseHandle(reinterpret_cast<HANDLE>(handle)); }

// The same two sinks every [NEVR.CRASH] line has: standard error and the crash record, through the handler's own writer.
void LogSummary(const char* line) {
  char text[nevr_crash::kSummaryCapacity + 16];
  const int length = std::snprintf(text, sizeof(text), "[NEVR.CRASH] %s", line);
  if (length > 0) WriteCrashRecordLine(text);
}

const nevr_crash::Platform kPlatform = {&ResolveAddress, &Describe,  &ReportPath,  &OpenFile,
                                        &WriteFileAll,   &CloseFile, &RenameFile, &LogSummary};

}  // namespace

void RegisterCrashReportPlatform(const char* crashes_dir, ModuleLookupFn lookup) {
  g_lookup = lookup;
  g_stem[0] = '\0';
  if (crashes_dir != nullptr && crashes_dir[0] != '\0') {
    const int length = std::snprintf(g_stem, sizeof(g_stem), "%s\\nevr-crash-%s", crashes_dir, GetRunId());
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(g_stem)) g_stem[0] = '\0';
  }
  nevr_crash::SetPlatform(&kPlatform);
}

}  // namespace nevr_crash_win

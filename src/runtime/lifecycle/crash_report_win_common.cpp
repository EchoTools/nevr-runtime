#include "runtime/lifecycle/crash_report_win_common.h"

#include <cstring>

namespace nevr_crash_win {

namespace {

constexpr int kStackScanQwords = 512;

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

}  // namespace

const char* ExceptionCodeName(DWORD code) {
  for (const NamedCode& entry : kCodes) {
    if (entry.code == code) return entry.name;
  }
  return "unknown exception";
}

void DescribeException(const nevr_crash::CrashInfo& info, nevr_crash::Report* report, ReadableFn readable,
                       KnownAddressFn known) {
  report->signal_name = ExceptionCodeName(static_cast<DWORD>(info.code));
  report->signal_number_hex = true;
  report->has_lr = false;

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
    if (readable == nullptr || !readable(sp + i, 8)) break;
    if (known != nullptr && known(sp[i])) report->frames[count++] = sp[i];
  }
  report->frame_count = count;
}

bool RenameReportFile(const char* from, const char* to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0; }

std::intptr_t OpenReportFile(const char* path) {
  const HANDLE handle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  return handle == INVALID_HANDLE_VALUE ? -1 : reinterpret_cast<std::intptr_t>(handle);
}

bool WriteReportFile(std::intptr_t handle, const char* data, std::size_t size) {
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

void CloseReportFile(std::intptr_t handle) { CloseHandle(reinterpret_cast<HANDLE>(handle)); }

}  // namespace nevr_crash_win

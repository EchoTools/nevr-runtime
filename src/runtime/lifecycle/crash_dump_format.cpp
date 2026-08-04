#include "runtime/lifecycle/crash_dump_format.h"

#include <cstdio>

namespace CrashRecovery {
namespace {

const char* ExceptionName(uint32_t exception_code) {
  switch (exception_code) {
    case 0xC0000005: return "ACCESS_VIOLATION";
    case 0x80000003: return "BREAKPOINT";
    case 0xC000001D: return "ILLEGAL_INSTRUCTION";
    case 0xC00000FD: return "STACK_OVERFLOW";
    case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
    case 0x20474343: return "CXX_THROW_FROM_NEVR_DLL";
    default: return "Unknown";
  }
}

}  // namespace

int FormatCrashExceptionSummary(char* buffer, size_t buffer_size,
                                uint32_t exception_code, uint64_t rip,
                                uint64_t game_base, uint32_t thread_id) {
  if (buffer == nullptr || buffer_size == 0) return 0;
  const bool in_game = rip >= game_base && rip < game_base + 0x2000000ULL;
  const uint64_t location = in_game ? rip - game_base : rip;
  const int result = std::snprintf(
      buffer, buffer_size,
      "[NEVR.CRASH] exception name=%s code=0x%08lX rip=0x%llX rip_rva=%s0x%llX tid=%lu",
      ExceptionName(exception_code), static_cast<unsigned long>(exception_code),
      static_cast<unsigned long long>(rip), in_game ? "game+" : "external:",
      static_cast<unsigned long long>(location), static_cast<unsigned long>(thread_id));
  return result < 0 ? 0 : result;
}

}  // namespace CrashRecovery

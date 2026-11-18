#pragma once

// What the PC crash-first filter hands to the shared report writer and writes to the crash record (#1605), as pure functions of
// the exception the system raised, so a test can drive them without the hook, MinHook or the game (crash_first_win.cpp is the
// wiring that calls them). Both run in the exception context: no allocation, no Log() and no lock (N70).

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "core/crash_report.h"

namespace nevr_crash_first_win {

/// The size of the buffer the first_handler line is built in (the longest line is well under it; a longer one is cut).
constexpr std::size_t kRecordLineSize = 192;

/// The CrashInfo of the first fatal exception: its code and address from the exception record, the fault address of an access
/// violation or in-page error (the record's second parameter), the context's instruction pointer as the pc when there is one
/// (it is the faulting instruction; the record's address can be a function that raised it), and the context itself for the
/// registers and the frames. A null `info` or null parts give the fields they would have held as 0.
inline nevr_crash::CrashInfo BuildFirstFatalInfo(EXCEPTION_POINTERS* info, std::uint64_t threadId) noexcept {
  constexpr DWORD kFaultAddressParameter = 1;  // ExceptionInformation[1] is the address of an AV or in-page error
  nevr_crash::CrashInfo crash{};
  crash.thread_id = threadId;
  if (info != nullptr && info->ExceptionRecord != nullptr) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    crash.code = static_cast<std::int64_t>(record->ExceptionCode);
    crash.pc = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record->ExceptionAddress));
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters > kFaultAddressParameter) {
      crash.fault_address = static_cast<std::uint64_t>(record->ExceptionInformation[kFaultAddressParameter]);
    }
  }
  if (info != nullptr && info->ContextRecord != nullptr) crash.pc = info->ContextRecord->Rip;
  crash.context = info;
  return crash;
}

/// The crash-record line written after the game's filter returned: the exception code (0 without a record), whether the game had a
/// filter of its own to run after ours, and how many times it asked to install one (each would have displaced ours). Written into
/// `line` (at most `size` bytes with the terminator, cut if longer).
inline void FormatFirstHandlerLine(char* line, std::size_t size, EXCEPTION_POINTERS* info, bool chained, long requests) noexcept {
  const unsigned long code =
      info != nullptr && info->ExceptionRecord != nullptr ? static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode) : 0UL;
  std::snprintf(line, size, "[NEVR.CRASH] first_handler code=0x%08lX game_filter_called=%d displaced_requests=%ld", code,
                chained ? 1 : 0, requests);
}

}  // namespace nevr_crash_first_win

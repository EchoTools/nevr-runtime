#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "core/crash_report.h"

// The parts of the PC crash report (core/crash_report.h, #1605) that do not depend on who is writing it. Two writers use
// them: the runtime (crash_report_win.cpp, which knows the runtime's module snapshot, the game-loop thread and the crash
// record) and the BugSplat64.dll host (host/crash_report_host.cpp, for a crash the runtime is not there to report, #1689).
// One implementation of what a PC report says about an exception and how the file is written.
//
// Crash path (N70): raw kernel32 file calls, no allocation, no lock, no logger, no module enumeration. The module test and
// the memory probe are the caller's.

namespace nevr_crash_win {

// True when [address, address + length) can be read. The caller's probe (a VirtualQuery check).
using ReadableFn = bool (*)(const void* address, std::size_t length);
// True when `address` falls inside a module the caller knows (a return address worth listing in the backtrace).
using KnownAddressFn = bool (*)(std::uint64_t address);

/// The name a report prints for an exception code (`unknown exception` for one it has no name for).
const char* ExceptionCodeName(DWORD code);

/// Fills the exception's part of `report` from `info.context` (an EXCEPTION_POINTERS*): the signal name, no link
/// register, the fault address and kind of an access violation, the x64 registers, and a backtrace made of the faulting
/// rip and then the words on the stack that point into a known module (x64 keeps no frame chain). A stack overflow lists
/// the rip only. A null context leaves the rest of `report` as it is.
void DescribeException(const nevr_crash::CrashInfo& info, nevr_crash::Report* report, ReadableFn readable,
                       KnownAddressFn known);

// The file seams of nevr_crash::Platform, over kernel32: create or truncate, write all and flush, close, rename over.
std::intptr_t OpenReportFile(const char* path);
bool WriteReportFile(std::intptr_t handle, const char* data, std::size_t size);
void CloseReportFile(std::intptr_t handle);
bool RenameReportFile(const char* from, const char* to);

}  // namespace nevr_crash_win

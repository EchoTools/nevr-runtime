#pragma once

// BootLogTee — early-boot tee into the process's main log.
//
// During the boot phase, before CLog::PrintfImpl is hooked, all boot progress messages go to stderr. BootLogTee also appends each
// one to the process's one main log, <nevr root>\logs\nevr-<process creation time>.jsonl (core/run_log_path.h), as a JSON record
// marked "src":"boot" through run_log_writer.h, the writer the log filter goes on using after the boot phase, so a run reads as one
// file in time order and there is no boot file and nothing to replay. It uses only Win32 kernel32 APIs (CreateFileA / WriteFile) —
// no CRT fd table or heap-allocated stream (it was written for the loader lock; initialization now runs from the host's wrapper
// detour, where the game's CLog still does not exist, and the tee keeps the same rules).
//
// DESIGN DECISION (N43): The addendum forbids opening file handles from DllMain. Initialize() no longer runs there (it runs from
// the host's wrapper detour), but the writer still opens the file in the boot phase, deliberately, documented as an exception to the
// loader-lock-safe rules, because the boot phase of Initialize() runs before the game's own log exists and the only alternative is a
// crash-only post-mortem. The kernel32 API path avoids CRT locks entirely.

namespace nevr_boot_log_tee {

/// Call ONCE before the first TeeFprintf.  Names the process's main log (the writer opens it on the first record).
/// Starts the boot phase: from here to Close(), Log() reports through this tee
/// (with the level after the subsystem tag, logging.md Rule 12), never the
/// game's CLog, which does not exist yet.
/// If the folder cannot be made or the file cannot be opened (no %LOCALAPPDATA%, read-only filesystem, permissions), the writer says so on
/// stderr, counts the lost records, and every TeeFprintf degrades to stderr-only; nothing is written anywhere else.
void Init();

/// Drop-in replacement for fprintf(stderr, fmt, ...).
/// Writes to stderr (always) AND to the main log (if it can be written).
void TeeFprintf(const char* fmt, ...);

/// True from Init() to Close(): the boot phase, in which Log() reports through this tee. Code that must
/// not call the game's CLog under the loader lock asks this to know whether Log() is safe (#654).
bool Active();

/// Full path of the process's main log once the writer has resolved it ("" before, or when the folder could not be built). The
/// filter adopts the same file.
const char* Path();

/// Ends the boot phase (Log() returns to the game's CLog). The main log stays open: the filter goes on writing it. After this call
/// TeeFprintf still writes to stderr but the file mirror stops.
void Close();

}  // namespace nevr_boot_log_tee

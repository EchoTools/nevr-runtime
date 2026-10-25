#pragma once

// BootLogTee — early-boot fprintf tee to JSONL log file.
//
// During DLL_PROCESS_ATTACH, before CLog::PrintfImpl is hooked, all boot
// progress messages go through fprintf(stderr, ...).  Those messages never
// reach the rotating JSONL log file, which makes diagnosing early-boot
// failures (missing modules, hook failures, prologue mismatches) blind.
//
// BootLogTee opens a separate nevr-boot.jsonl alongside the EXE and
// mirrors every TeeFprintf() call there in JSONL format, using only Win32
// kernel32 APIs (CreateFileA / WriteFile / CloseHandle) — no CRT fd table
// or heap allocation (it was written for the loader lock; initialization now runs from the host's wrapper detour,
// where the game's CLog still does not exist, and the tee keeps the same rules).
//
// DESIGN DECISION (N43): The addendum forbids opening file handles from
// DllMain.  Initialize() no longer runs there (it runs from the host's wrapper
// detour), but this module still opens its file in the boot phase, deliberately,
// documented as an exception to the loader-lock-safe rules,
// because the boot phase of Initialize() runs before the game's own log exists
// and the only alternative is a crash-only post-mortem.  The kernel32 API path
// avoids CRT locks entirely; WriteFile to a simple log file does not
// trigger loader-lock re-entry through filesystem filter drivers in
// practice.

namespace nevr_boot_log_tee {

/// Call ONCE before the first TeeFprintf.  Creates logs/nevr-boot.jsonl
/// alongside the EXE (appending if it already exists from a prior run).
/// Starts the boot phase: from here to Close(), Log() reports through this tee
/// (with the level after the subsystem tag, logging.md Rule 12), never the
/// game's CLog, which does not exist yet.
/// Silently ignores errors — if the directory cannot be created or the
/// file cannot be opened (read-only filesystem, permissions), all
/// TeeFprintf calls degrade to stderr-only.
void Init();

/// Drop-in replacement for fprintf(stderr, fmt, ...).
/// Writes to stderr (always) AND to the boot JSONL file (if open).
void TeeFprintf(const char* fmt, ...);

/// True from Init() to Close(): the boot phase, in which Log() reports through this tee. Code that must
/// not call the game's CLog under the loader lock asks this to know whether Log() is safe (#654).
bool Active();

/// Full path of nevr-boot.jsonl once Init() has resolved it ("" before, or when the directory could
/// not be built). Valid after Close(): the main log replays this run's lines from it (#5).
const char* Path();

/// Ends the boot phase (Log() returns to the game's CLog) and closes the boot
/// file handle.  After this call TeeFprintf still writes to stderr but the
/// file mirror stops.
void Close();

}  // namespace nevr_boot_log_tee

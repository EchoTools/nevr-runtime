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
// or heap allocation, safe under the loader lock on a static-CRT build.
//
// DESIGN DECISION (N43): The addendum forbids opening file handles from
// DllMain.  This module does it deliberately, documented as an exception,
// because the entire Initialize() sequence runs under the loader lock and
// the only alternative is a crash-only post-mortem.  The kernel32 API path
// avoids CRT locks entirely; WriteFile to a simple log file does not
// trigger loader-lock re-entry through filesystem filter drivers in
// practice.

namespace BootLogTee {

/// Call ONCE before the first TeeFprintf.  Creates logs/nevr-boot.jsonl
/// alongside the EXE (appending if it already exists from a prior run).
/// Silently ignores errors — if the directory cannot be created or the
/// file cannot be opened (read-only filesystem, permissions), all
/// TeeFprintf calls degrade to stderr-only.
void Init();

/// Drop-in replacement for fprintf(stderr, fmt, ...).
/// Writes to stderr (always) AND to the boot JSONL file (if open).
void TeeFprintf(const char* fmt, ...);

/// Close the boot file handle.  After this call TeeFprintf still writes
/// to stderr but the file mirror stops.
void Close();

}  // namespace BootLogTee

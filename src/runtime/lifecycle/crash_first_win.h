#pragma once

// The wiring of the PC crash-first filter (#1605; the logic is crash_first_filter.h): hooks kernel32's
// SetUnhandledExceptionFilter so the game's request for a crash filter is remembered and not installed, installs
// TopLevelFilter as the top-level filter, and hands the first fatal exception to the shared report writer
// (core/crash_report.h) before the game's own filter runs.

// Installs the hook and the filter. Idempotent; false (with a Warning that names the cause) when the hook could not be
// made, in which case the game's filter replaces ours as before. Call it after MinHook is initialised, from the
// runtime's crash hooks (InstallCrashRecoveryHooks).
bool InstallCrashFirstFilter();

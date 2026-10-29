// cli_reject.h: ending a run for a command line it cannot use (#1255; #1253 extends this).
//
// One rejection is one JSON line on stderr, the same text in the run's log at Error under [NEVR.STOP], and exit code 64
// (EX_USAGE: a supervisor must not restart on it). It happens before anything the game needs is started. There is no dialog
// on any run, no pause and no retry.
#pragma once

#include <string>

namespace nevr_cli_reject {

/// The exit code of every rejection.
inline constexpr unsigned kExitCode = 64;

/// The version part of an identity literal ("NEVR-BUILD <version> <commit40>"); empty when it is not one.
std::string VersionOfIdentity(const char* identity_literal);

/// The stop line: a single-line JSON object with tag NEVR.STOP, ts, pid, event stop, stopped true, code 64, reason usage,
/// runtime_version and detail ("<argument>: <why>"). Built with nlohmann::json; no side effect.
std::string BuildStopLine(const std::string& argument, const std::string& why, const std::string& runtime_version,
                          const std::string& timestamp, unsigned long pid);

}  // namespace nevr_cli_reject

/// Writes the stop line for `argument` and `why` to stderr and to the log, then ends the process with exit code 64
/// through ForceFatalExit (which works in server mode, where ExitProcess is suppressed). Does not return.
[[noreturn]] void RejectCommandLine(const std::string& argument, const std::string& why);

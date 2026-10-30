#pragma once

// The one console line the host writes whenever it stops the game, or leaves the game running without nevr-runtime.dll
// (tag `NEVR.STOP`, #1252). A `-server` operator has a console and no dialog; this is what they read, and the same facts
// go to the host log (nevr-bootstrap.jsonl) so the two agree. The line is built in a fixed buffer with JsonLine
// (host_json.h) and written with one WriteFile to the standard error handle: no heap, no lock, safe in the places the
// host stops the game from. Nothing here calls the NEVR logger.
//
// Fields, in this order:
//   tag             "NEVR.STOP"
//   ts, pid         as every host record
//   event           "stop"
//   stopped         true when the host ends the process; false when the game keeps running without the runtime
//   code            the exit code (78 for every runtime that did not load; 0xE0420003 for the image guard), as a
//                   number; omitted when stopped is false
//   reason          a token: runtime_absent, runtime_refused, dependency_load_failed, load_failed, missing_entry,
//                   null_descriptor, abi_rejected, init_failed, no_path, unsupported_image or host_not_attached
//   host_version    the version in the host's own NEVR-BUILD literal
//   runtime_version the version in nevr-runtime.dll's NEVR-BUILD literal when the host read the file, else "not loaded"
//   detail          what is wrong and where, in words
//   log             the full path of the host log the same facts were written to ("" when no log file took them)
// docs/standards/logging.md lists the tag and the exit codes.

#include "runtime/host/bootstrap_diagnostics.h"
#include "runtime/host/runtime_load.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include <nlohmann/json_fwd.hpp>

namespace nevr_host {

inline constexpr char kStopTag[] = "NEVR.STOP";
inline constexpr char kRuntimeNotLoaded[] = "not loaded";
inline constexpr char kReasonUnsupportedImage[] = "unsupported_image";
inline constexpr char kReasonHostNotAttached[] = "host_not_attached";

/// The room a line has. A path of MAX_PATH characters is at most 780 bytes in UTF-8; the longest detail holds three of
/// them (the path searched, the directory of the host and the directory to move a legacy runtime to) and its words, so
/// it has 2944 bytes. A piece of detail that does not fit is left out whole and the detail ends with `[detail
/// incomplete: a path did not fit]`. The line holds the detail and the log path, each escaped (a backslash is two bytes),
/// and a line that still does not fit is replaced whole by JsonLine with a marker line (`record_too_large`).
inline constexpr std::size_t kStopLineBytes = 8192;
inline constexpr std::size_t kStopDetailBytes = 2944;

/// The closing text of a detail from which a piece was left out.
inline constexpr char kStopDetailIncompleteSuffix[] = "[detail incomplete: a path did not fit]";

struct StopLineFacts {
  bool stopped = true;
  std::uint32_t code = 0;                   ///< read only when `stopped`
  const char* reason = "";
  const char* host_version = "";
  const char* runtime_version = nullptr;    ///< null or empty: "not loaded"
  const char* detail = "";                  ///< UTF-8
  const wchar_t* log = nullptr;             ///< null or empty: ""
};

/// The line, with its newline, into `buffer`. Returns its length, or 0 when `buffer` cannot hold even the header.
std::size_t BuildStopLine(char* buffer, std::size_t capacity, const StopLineFacts& facts) noexcept;

/// BuildStopLine into a stack buffer, then one WriteFile to the standard error handle. False when no line could be
/// built or there is no standard error handle to write to.
bool PutStopLine(const StopLineFacts& facts) noexcept;

/// The facts of a runtime that did not load, with the text they point into. Not copyable: `facts` points into `detail`
/// and `log`.
struct RuntimeStopText {
  char detail[kStopDetailBytes] = {};
  wchar_t log[MAX_PATH] = {};
  StopLineFacts facts;
  RuntimeStopText() = default;
  RuntimeStopText(const RuntimeStopText&) = delete;
  RuntimeStopText& operator=(const RuntimeStopText&) = delete;
};

/// Fill `out` for `result`: the reason is the outcome's name, the code is kRuntimeStopExitCode, the detail says what is
/// wrong and the path searched, and, when the runtime is absent from its root and a nevr-runtime.dll sits beside
/// BugSplat64.dll (the old layout), it says the host does not load that one and where to move it. `diagnostics` supplies
/// the host's directory and the log path. No heap.
void DescribeRuntimeStop(const BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result,
                         RuntimeStopText& out) noexcept;

/// DescribeRuntimeStop, then PutStopLine: the line for a runtime that did not load. Call it after the `game_stop`
/// record is written, so the `log` field names the file that took the record. False when no line could be written.
bool WriteRuntimeStopLine(const BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result) noexcept;

/// The room the plain notice line has. It holds `sentence`, the detail and the log path.
inline constexpr std::size_t kNoticeLineBytes = 8192;

/// The plain line a person reads when the runtime did not load (#1533): `sentence` (what failed and the fix), then the
/// detail, then the log path, as one line of text, no JSON. It goes to the first console that takes it (PutConsoleText),
/// so a `-server` started from a shell is told in its own console. A `console_notice` record says whether one took it.
/// Returns true when a console took the line. Call it after the `game_stop` record, like WriteRuntimeStopLine.
bool WriteRuntimeNoticeLine(const wchar_t* sentence, BootstrapDiagnostics& diagnostics,
                            const RuntimeLoadResult& result) noexcept;

/// Add the facts a record of the host log shares with the line: `stopped`, `host_version`, `runtime_version`, `detail`
/// and `log`. The record keeps its own `event`, `reason` and `exit_code`.
void AddStopFacts(nlohmann::json& record, const StopLineFacts& facts);

/// The detail a failed attach step carries on the stop line and in the host log.
const char* AttachFailureDetail(AttachFailure failure) noexcept;

/// The game image is not a supported echovr.exe: the host ends the process with `exit_code` (0xE0420003). Writes the
/// `game_stop` record, then the line. Called just before the image guard ends the process.
void ReportUnsupportedImage(BootstrapDiagnostics& diagnostics, std::uint32_t exit_code) noexcept;

/// The attach ended with no detour, so the game runs without the runtime. Writes the `host_not_attached` record and the
/// line (stopped=false, no code), naming the step that failed. Nothing when no step failed.
void ReportNotAttached(BootstrapDiagnostics& diagnostics) noexcept;

}  // namespace nevr_host

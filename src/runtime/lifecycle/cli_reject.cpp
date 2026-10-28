// cli_reject.cpp: see cli_reject.h.

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include "runtime/lifecycle/cli_reject.h"

#include <cstdlib>
#include <exception>

#include <nlohmann/json.hpp>

#include "abi/echovr.h"
#include "core/build_identity.h"
#include "core/logging.h"
#include "runtime/lifecycle/crash_recovery.h"

namespace nevr_cli_reject {

std::string VersionOfIdentity(const char* identity_literal) {
  if (identity_literal == nullptr) return std::string();
  const std::string text(identity_literal);
  const std::string prefix = "NEVR-BUILD ";
  if (text.compare(0, prefix.size(), prefix) != 0) return std::string();
  const std::size_t end = text.find(' ', prefix.size());
  return text.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
}

std::string BuildStopLine(const std::string& argument, const std::string& why, const std::string& runtime_version,
                          const std::string& timestamp, unsigned long pid) {
  nlohmann::ordered_json line;
  line["tag"] = "NEVR.STOP";
  line["ts"] = timestamp;
  line["pid"] = pid;
  line["event"] = "stop";
  line["stopped"] = true;
  line["code"] = kExitCode;
  line["reason"] = "usage";
  line["runtime_version"] = runtime_version;
  line["detail"] = argument + ": " + why;
  // `replace`: a value that is not valid UTF-8 becomes U+FFFD, and the line is still written (dump would throw).
  return line.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

}  // namespace nevr_cli_reject

namespace {

// The text and a newline to the process's standard error, when it has one (a console-less process may not).
void WriteToStandardError(const std::string& text) {
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err == nullptr || err == INVALID_HANDLE_VALUE) return;
  const std::string out = text + "\n";
  DWORD written = 0;
  WriteFile(err, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
}

}  // namespace

void RejectCommandLine(const std::string& argument, const std::string& why) {
  try {
    const std::string line = nevr_cli_reject::BuildStopLine(
        argument, why, nevr_cli_reject::VersionOfIdentity(nevr_build_identity::IdentityLiteral()), GetISO8601Timestamp(),
        GetCurrentProcessId());
    WriteToStandardError(line);
    Log(EchoVR::LogLevel::Error, "[NEVR.STOP] %s", line.c_str());
  } catch (const std::exception&) {
    Log(EchoVR::LogLevel::Error, "[NEVR.STOP] the stop line could not be built (std::exception); ending the run with code %u",
        nevr_cli_reject::kExitCode);
  }
  ForceFatalExit(nevr_cli_reject::kExitCode);
  std::abort();
}

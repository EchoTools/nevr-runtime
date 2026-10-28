// cli_reject.cpp: see cli_reject.h.

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include <cstdlib>

#include "runtime/lifecycle/cli_reject.h"
#include "runtime/lifecycle/crash_recovery.h"

namespace nevr_cli_reject {

std::string VersionOfIdentity(const char* identity_literal) {
  (void)identity_literal;
  return std::string();
}

std::string BuildStopLine(const std::string& argument, const std::string& why, const std::string& runtime_version,
                          const std::string& timestamp, unsigned long pid) {
  (void)argument;
  (void)why;
  (void)runtime_version;
  (void)timestamp;
  (void)pid;
  return std::string();
}

}  // namespace nevr_cli_reject

void RejectCommandLine(const std::string& argument, const std::string& why) {
  (void)argument;
  (void)why;
  ForceFatalExit(1);
  std::abort();
}

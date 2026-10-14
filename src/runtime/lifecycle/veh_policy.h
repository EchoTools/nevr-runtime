#pragma once

#include <cstdint>

// Whether InstallVEH installs the vectored exception handler, and the boot-log line that says what
// it did. Pure, so both are unit-tested without the process-wide handler in crash_recovery.cpp.
//
// A Wine client keeps Wine's own first-chance stack-overflow handling: a first-priority foreign VEH
// changes that handler ordering even when it returns CONTINUE_SEARCH. Server mode is unknown during
// early initialization, so Wine installation is retried after command-line preflight.

namespace nevr_veh_policy {

enum class InstallResult { Installed, DeferredWine, Failed };

inline bool ShouldInstall(bool isServer, bool isWineClient) { return isServer || !isWineClient; }

inline bool ShouldRecoverNullPointerAv(bool isServer, uint64_t target, bool jumpBufferValid,
                                       uint32_t currentThreadId, uint32_t gameLoopThreadId) {
  return isServer && target < 0x10000 && jumpBufferValid && gameLoopThreadId != 0 &&
         currentThreadId == gameLoopThreadId;
}

// The line initialize.cpp writes before CLI parsing. A Wine process may later become a server.
inline const char* BootLine(InstallResult result) {
  switch (result) {
    case InstallResult::Installed: return "[NEVR.CRASH] veh installed\n";
    case InstallResult::DeferredWine: return "[NEVR.CRASH] veh deferred reason=wine_cli_unknown\n";
    case InstallResult::Failed: return "[NEVR.CRASH] veh install failed\n";
  }
  return "[NEVR.CRASH] veh install failed\n";
}

}  // namespace nevr_veh_policy

#pragma once

#include <cstdint>
#include <cstring>

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

// N70: the module names whose faults the crash handler attributes to NEVR. The runtime is two images, the host
// the game imports (BugSplat64.dll) and the runtime it loads (nevr-runtime.dll); the handler runs in the second,
// and a fault in either is a NEVR fault. The cache of loaded modules is deliberately comprehensive (Wine and
// DXVK are in it), so a range match alone means only "some loaded DLL": the name decides. A plugin or a foreign
// DLL is not in this set.
inline bool IsNevrImageName(const char* name) {
  return name != nullptr && (_stricmp(name, "BugSplat64.dll") == 0 || _stricmp(name, "nevr-runtime.dll") == 0);
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

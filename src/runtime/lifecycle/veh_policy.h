#pragma once

// Whether InstallVEH installs the vectored exception handler, and the boot-log line that says what
// it did. Pure, so both are unit-tested without the process-wide handler in crash_recovery.cpp.
//
// A Wine client keeps Wine's own first-chance stack-overflow handling: a first-priority foreign VEH
// changes that handler ordering even when it returns CONTINUE_SEARCH. The handler is server-only
// recovery, so it is skipped on a Wine client and installed everywhere else.

namespace VehPolicy {

inline bool ShouldInstall(bool isServer, bool isWineClient) { return isServer || !isWineClient; }

// The line initialize.cpp writes to the boot log after InstallVEH returns. `installed` is the
// result InstallVEH returned, never an assumption.
inline const char* BootLine(bool installed) {
  return installed ? "[NEVR.CRASH] veh installed\n"
                   : "[NEVR.CRASH] veh skipped reason=wine_client\n";
}

}  // namespace VehPolicy

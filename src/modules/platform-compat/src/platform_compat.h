// platform_compat.h: what the runtime calls in platform_compat before the module's own init.

#pragma once

namespace nevr_platform_compat {

// Installs the CreateDirectoryW and CreateDirectoryA hooks and the game folder redirect (#575), once per process.
// boot.cpp calls it before the game's original PreprocessCommandLine, so the game's first _local and _temp access is
// already redirected; platform_compat_Init reports the CreateDirectory result as createdir. True when both
// CreateDirectory hooks installed.
bool InstallEarlyFileHooks();

}  // namespace nevr_platform_compat

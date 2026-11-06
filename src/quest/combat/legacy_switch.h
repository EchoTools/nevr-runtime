/* The q15t plugin's kill files, which the folded modules no longer read (docs/adr/0046 C2: a module's switch is a key under `combat:` in
 * config.yaml, never a file). A module that had one looks for it at install and says so when it is there, naming the key that replaces it,
 * so an operator who relied on a file learns why the module is running. Header only; used by the modules that moved from the plugin.
 */
#pragma once

#include <unistd.h>

#include <cstddef>
#include <cstdio>

#include "quest/sentinel/sentinel_log.h"

namespace nevr_quest::combat {

// The plugin's data directory below the sentinel's files folder (sentinel::FilesDir()), where its switch files were read from.
inline constexpr const char* kLegacySwitchSubdir = "q15t";

// True when `<dir>/<file>` exists. `dir` null: not looked for.
inline bool LegacySwitchFilePresent(const char* dir, const char* file) noexcept {
  if (dir == nullptr || file == nullptr) return false;
  char path[360];
  const int wrote = std::snprintf(path, sizeof(path), "%s/%s", dir, file);
  return wrote >= 0 && static_cast<std::size_t>(wrote) < sizeof(path) && access(path, F_OK) == 0;
}

// The production directory into `buf`; false when it does not fit.
inline bool LegacySwitchDir(char* buf, std::size_t size) noexcept {
  const int wrote = std::snprintf(buf, size, "%s/%s", sentinel::FilesDir(), kLegacySwitchSubdir);
  return wrote >= 0 && static_cast<std::size_t>(wrote) < size;
}

}  // namespace nevr_quest::combat

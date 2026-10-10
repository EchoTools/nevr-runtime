#pragma once
// Writes `data` to `path` atomically: a fresh temp file (O_EXCL, O_NOFOLLOW, mode 0600), fsync, rename over
// `path`, fsync of the directory. On any failure before the rename the temp file is removed and `path` is
// untouched. A failed directory fsync after the rename is reported through `warning` only: the new file is
// in place. Shared by the credential cache (file_store.cpp) and the hardware dump (quest/diag).

#include <string>

namespace nevr::quest_auth {

bool AtomicWrite(const std::string& path, const std::string& data, std::string& error, std::string& warning);

}  // namespace nevr::quest_auth

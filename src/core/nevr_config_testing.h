// nevr_config_testing.h — the one seam the NevrConfig::Set tests need: the rename that publishes the temporary file. A test
// installs a rename that fails to prove the previous file is left intact. Not for production code.
#pragma once

namespace nevr::config_testing {

/// Replaces the file rename NevrConfig::Set ends with: (from, to) -> 0 on success, else an errno-style code. nullptr restores
/// the real one (std::filesystem::rename, which replaces an existing file on both POSIX and Windows).
using RenameFn = int (*)(const char* from, const char* to);
void SetRenameForTest(RenameFn rename);

}  // namespace nevr::config_testing

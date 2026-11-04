// nevr_config_testing.h — the seams the NevrConfig::Set tests need: the rename that publishes the temporary file (a test installs a
// rename that fails to prove the previous file is left intact) and a call made just before the in-memory tree is updated (a test
// holds one Set there to prove no other Set gets in between). Not for production code.
#pragma once

namespace nevr::config_testing {

/// Replaces the file rename NevrConfig::Set ends with: (from, to) -> 0 on success, else an errno-style code. nullptr restores
/// the real one (std::filesystem::rename, which replaces an existing file on both POSIX and Windows).
using RenameFn = int (*)(const char* from, const char* to);
void SetRenameForTest(RenameFn rename);

/// Called by NevrConfig::Set after the file holds the value and before the in-memory tree is updated, on the calling thread. nullptr
/// removes it. With the per-file lock held across the update, no other Set on that file runs while it waits.
using BeforeMemoryUpdateFn = void (*)();
void SetBeforeMemoryUpdateForTest(BeforeMemoryUpdateFn hook);

}  // namespace nevr::config_testing

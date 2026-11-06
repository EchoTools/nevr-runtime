// nevr_config_testing.h — the seams the NevrConfig::Set tests need: the rename that publishes the temporary file (a test installs a
// rename that fails to prove the previous file is left intact) and a call made after the in-memory tree is updated, still under the file's
// lock (a test holds one Set there to prove the lock is held through the update). Not for production code.
#pragma once

namespace nevr::config_testing {

/// Replaces the file rename NevrConfig::Set ends with: (from, to) -> 0 on success, else an errno-style code. nullptr restores
/// the real one (std::filesystem::rename, which replaces an existing file on both POSIX and Windows).
using RenameFn = int (*)(const char* from, const char* to);
void SetRenameForTest(RenameFn rename);

/// Called by NevrConfig::Set on the calling thread after the file holds the value and the in-memory tree is updated, just before it
/// returns, with the file's lock still held. nullptr removes it. With the lock held through the update, no other Set on that file
/// runs while it waits. A hook that calls Set on the same file would deadlock on that lock: test-only, never do it.
using AfterMemoryUpdateFn = void (*)();
void SetAfterMemoryUpdateForTest(AfterMemoryUpdateFn hook);

}  // namespace nevr::config_testing

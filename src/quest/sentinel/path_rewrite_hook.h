/* The data-path rewrite's hooks (#1737; the rewrite itself is path_rewrite.h).
 *
 * Installed only when libr15_patch::Attach accepted the pinned service-only file (AttachStatus::kPinnedServiceOnly), and then
 * only on that file: every target is GotTarget::ServiceSide(), and nothing else installs them. Eight GOT import slots of
 * libr15.so, the libc calls through which CSysFile and the Wwise I/O hook reach a path (a scan of every `bl <sym@plt>` in the
 * pinned file): open (CSysFile::Open, OpenRead, FileExists), fopen (CWwiseLowLevelIO::Open), access (CSysFile::Init), mkdir
 * (CSysFile::CreateDir, AttemptCreateDir), opendir (CSysFile::EnumerateDirectory), remove (CSysFile::Delete), rename
 * (CSysFile::RenameFile, both paths) and rmdir (CSysFile::RemoveDir). stat is not hooked: no CSysFile function calls it.
 *
 * Cost: these are per file-system call, not per frame or per read (a package is opened, then read through its descriptor).
 * A call that is not under /sdcard/Android/media/ costs one strncmp of at most 22 bytes; a call under it adds one of 19
 * bytes; a rewritten call adds a copy of the path into a 640-byte stack buffer.
 *
 * Log: one `path_rewrite_install` line per slot (symbol, slot, status) and one summary line; the first rewrite of each
 * distinct directory (up to 32) at Info as `path_rewrite_first`; `path_rewrite_counts` (per-symbol totals) at most once
 * every 10 s, written from the call that crosses the 10 s mark.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

namespace nevr_quest::path_rewrite {

// Reads the running package from /proc/self/cmdline, then installs the eight hooks. With the running package equal to
// com.readyatdawn.afk (or unreadable) nothing is installed and one line says so. True when all eight took.
bool InstallForServiceOnly() noexcept;

}  // namespace nevr_quest::path_rewrite

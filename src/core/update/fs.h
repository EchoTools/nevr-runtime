#pragma once

// The files of update/ as the self-update engine sees them (docs/adr/0024-pc-self-update.md, sections 2 and 4), behind
// one interface so the engine is the same on every platform and only the calls that touch a file differ: Win32 on PC
// (runtime/lifecycle/update_fs_win32.h), POSIX on Quest and on the Linux host tests (fs_posix.cpp). The engine composes every
// path from the nevr root and the closed vocabulary of names.h, and hands this interface UTF-8 paths; an implementation
// converts them for its calls. Pure interface: no platform header, no I/O of its own.

#include <cstdint>
#include <memory>
#include <string>

namespace nevr_update {

enum class ReadResult {
  kOk,
  kAbsent,      ///< no such file (or no such folder on the way to it)
  kUnreadable,  ///< it is there and cannot be opened or read: another program holds it, access is denied
  kTooBig,      ///< larger than the cap (nevr_update_names::kMaxJsonBytes)
};

/// update/lock, held: exclusive, released when this object is destroyed. Whether the lock file remains after release is the
/// implementation's: Windows deletes it with the last handle, a POSIX flock file may stay. The host holds the same lock from its first read of
/// update/ until its runtime is mapped (ADR 0024, section 4.1), and the runtime takes it around every write to update/, never
/// while it downloads.
class FsLock {
 public:
  FsLock() = default;
  FsLock(const FsLock&) = delete;
  FsLock& operator=(const FsLock&) = delete;
  virtual ~FsLock() = default;
  /// Lets go of the lock now; the destructor does it when this was not called.
  virtual void Release() noexcept = 0;
};

class UpdateFs {
 public:
  virtual ~UpdateFs() = default;

  /// The separator the engine joins the parts of a path with.
  virtual char Separator() const noexcept = 0;

  /// A file of update/, at most nevr_update_names::kMaxJsonBytes.
  virtual ReadResult ReadSmallFile(const std::string& path, std::string* out) const noexcept = 0;

  /// Writes `text` to a temporary file beside `path`, flushes it and renames it over `path`: a reader never sees half a file.
  /// The temporary is `<path>.tmp-<pid>`, the spelling a start removes after a kill between the write and the rename.
  virtual bool WriteByRename(const std::string& path, const std::string& text) const noexcept = 0;

  /// Removes one file. False when it was not removed (absent counts as not removed).
  virtual bool RemoveFile(const std::string& path) const noexcept = 0;

  /// Removes a folder and what is in it. Never follows a link: a link is removed itself and not entered.
  virtual void RemoveTree(const std::string& path) const noexcept = 0;

  /// True when `path` is a link (a reparse point on Windows, a symbolic link elsewhere). False for a path that is absent. When the
  /// implementation cannot tell (out of memory), it answers true: the engine then refuses to write through the path instead of
  /// writing through a link.
  virtual bool IsLink(const std::string& path) const noexcept = 0;

  /// True when something (a file, a folder or a link) is at `path`. When the implementation cannot tell (out of memory), it
  /// answers true: the engine then waits (a release may be pending) instead of staging over one. A path that does not convert to what
  /// the implementation's calls take names nothing, so it is false.
  virtual bool Exists(const std::string& path) const noexcept = 0;

  /// Takes update/lock in `update_dir`, trying every `step_ms` for up to `timeout_ms`. Null when it was not taken, with the
  /// platform's error code in `*os_error` when it is not null.
  virtual std::unique_ptr<FsLock> TakeLock(const std::string& update_dir, std::uint32_t timeout_ms, std::uint32_t step_ms,
                                           int* os_error) const noexcept = 0;

  /// Moves `from` to `to` with no copy (ADR 0024, section 4.2): a rename, never a copy across volumes. False when it did not move:
  /// a file is where `to` is going (a failure, never overwritten), `from` is absent, or the two are on different volumes. The
  /// platform's reason goes to `*os_error` when it is not null: EXDEV on POSIX for a move across volumes, ERROR_NOT_SAME_DEVICE on
  /// Win32. The journal undoes a move that failed. An implementation that cannot move keeps this default: it moves nothing and
  /// reports no platform reason (`*os_error` = 0). The PC implementation (runtime/lifecycle/update_fs_win32.h) keeps it.
  virtual bool Move(const std::string& /*from*/, const std::string& /*to*/, int* os_error) const noexcept {
    if (os_error != nullptr) *os_error = 0;
    return false;
  }
};

/// The POSIX implementation (fs_posix.cpp), for Quest and the Linux host tests. Every path must lie under `root_dir` (the nevr
/// root, the folder that holds update/). A folder between the root and the path is reached without following a link: a link there
/// refuses the operation, and Exists and IsLink then answer that something is there, so the engine waits and writes nothing
/// through it. A path outside the root, or one with a `..` name, names nothing. Null when the implementation cannot be made (out
/// of memory).
std::unique_ptr<UpdateFs> MakePosixUpdateFs(const std::string& root_dir) noexcept;

}  // namespace nevr_update

// Sentinel logging: every line goes to logcat (tag NEVR-Sentinel) and is appended to the on-disk
// log `nevr-sentinel.log` in the app's external files directory as one JSON object per line.
// Callers pass key and feature names only, never configured values.
#pragma once

#include "quest/sentinel/quest_config.h"

#include <cstddef>
#include <string>

#include <sys/types.h>

namespace sentinel {

// The app's external files directory (compile-time default, overridable at build time).
const char* FilesDir();

// Writes `message` to logcat and the on-disk log. Open failures and write failures are each
// reported once to logcat at error level; logcat keeps receiving every line.
void Emit(nevr_quest::LogLevel level, const std::string& message);

// Closes the on-disk log; the next Emit reopens it. Failure reports already made are not repeated.
void CloseDiskLog();

using WriteFn = ssize_t (*)(int, const void*, std::size_t);

// Writes all `size` bytes, retrying short writes and EINTR. A zero-byte write counts as failure
// (`*err` = EIO). Returns false with `*err` set on failure.
bool WriteAll(int fd, const char* data, std::size_t size, int* err, WriteFn write);

}  // namespace sentinel

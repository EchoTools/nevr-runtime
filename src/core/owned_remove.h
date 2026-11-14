#pragma once

// The one guard every deleter uses: a deleter removes only the entries it recorded making, each one by name, one at
// a time. It never walks a tree, never deletes a name found by listing a folder, and never follows a link in any part
// of the path. A folder is removed only when it is empty.
//
// No platform header here: the implementations are src/core/owned_remove_win32.cpp (PC) and
// src/core/owned_remove_posix.cpp (Quest, and the Linux host tests). Entry grammar and the Win32 path comparison live in
// src/core/owned_remove_rules.h, which has no platform calls.

#include <string>
#include <vector>

namespace nevr_owned {

enum class Outcome {
  kRemoved,
  kAbsent,
  kNotEmpty,
  kRefusedName,
  kRefusedLink,
  kRefusedOutsideRoot,
  kFailed,
};

struct Result {
  std::string entry;
  Outcome outcome;
  int os_error;
};

// The Outcome's name, for logs and tests. Never null.
const char* OutcomeName(Outcome outcome) noexcept;

// `entry` is relative to `root`, '/'-separated, from the caller's own record of what it made; never a name found by
// listing. Refused (kRefusedName): empty, absolute, a drive or ':' anywhere, '\\', "" / "." / ".." components.
// Removes that one entry and nothing else; never recurses.
Result RemoveEntry(const std::string& root_utf8, const std::string& entry) noexcept;

// Each entry in the order given (files first, then folders, children before parents). A folder is removed only when
// empty (kNotEmpty otherwise, and what is in it stays). Every entry is tried; one Result each, in the same order.
// If a result cannot be allocated, the returned vector is shorter than `entries`: the entries after its last result
// were not tried.
std::vector<Result> RemoveOwned(const std::string& root_utf8, const std::vector<std::string>& entries) noexcept;

}  // namespace nevr_owned

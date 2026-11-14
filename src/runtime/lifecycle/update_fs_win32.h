#pragma once

// The PC implementation of nevr_update::UpdateFs (core/update/fs.h): the Win32 calls ADR 0024 names. Paths arrive as UTF-8
// and are converted to UTF-16 for each call; a path that does not convert makes the operation fail.

#include <string>

#include "core/update/fs.h"

namespace nevr_update {

/// UTF-8 to UTF-16, strictly: text that is not UTF-8 (MB_ERR_INVALID_CHARS) does not convert and the function returns false,
/// so a path never reaches a different file than the one it names. An empty string converts to an empty string.
bool Utf8ToWide(const std::string& utf8, std::wstring* out);

/// The one Win32 UpdateFs. It holds no state.
const UpdateFs& Win32UpdateFs() noexcept;

}  // namespace nevr_update

#pragma once

// The host's crash folder access for the crash report send (core/crash_send.h): the wide Win32 calls (kernel32 only) behind
// the sender's Files seam, so the host holds no std::filesystem and no iostream (#1983) and a folder under a user name with
// non-ASCII letters still opens. Paths cross the seam as UTF-8. None of these throws.

#include <string>

#include "core/crash_send.h"

namespace nevr_host {

/// The sender's file system for the host.
const nevr_crash_send::Files* CrashSendFiles();

/// UTF-8 for a NUL-terminated UTF-16 path; empty when it cannot be converted.
std::string Utf8FromWide(const wchar_t* wide);

}  // namespace nevr_host

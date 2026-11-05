#pragma once

// What the host reads from a file it is about to trust: its bytes, their SHA-256 and, for one of our builds, the version
// its "NEVR-BUILD <version> <commit40>" literal carries. Shared by the runtime load (the `versions` record) and the
// self-update swap (the staged files, the version of the runtime it moves out). CryptoAPI only (advapi32, which the
// host links for the verifier).

#include <windows.h>

#include <string>
#include <vector>

namespace nevr_host {

/// The whole file, read from offset 0 through `file`. False on any read failure or an empty file.
bool ReadWholeFile(HANDLE file, std::vector<unsigned char>* out);

/// SHA-256 of `bytes` as 64 lowercase hex characters. Empty on any failure.
std::string Sha256Hex(const std::vector<unsigned char>& bytes);

/// The version field of the file's NUL-bounded "NEVR-BUILD <version> <commit40>" literal (src/core and
/// host_identity.cpp). Empty when the file has none.
std::string BuildVersion(const std::vector<unsigned char>& bytes);

}  // namespace nevr_host

#pragma once

// The version in a build's NEVR-BUILD literal (core/build_identity.h): a NUL, "NEVR-BUILD ", the version, a space, the commit
// (40 hex) and a NUL. The Quest shim reads it from the runtime file it maps, through the file's descriptor and in bounded reads,
// so the file is never held whole. Header only; only Quest code includes it, so no PC object changes.

#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace nevr_build_literal {

// The bytes before the version: the NUL that starts the literal, then "NEVR-BUILD " (build_identity.h).
inline constexpr char kPrefix[] = {'\0', 'N', 'E', 'V', 'R', '-', 'B', 'U', 'I', 'L', 'D', ' '};
inline constexpr std::size_t kPrefixBytes = sizeof kPrefix;
// The longest version kept. A build's version is far shorter; a longer one is not read and the literal counts as absent.
inline constexpr std::size_t kMaxVersionBytes = 64;
// The commit that follows the version and its space: 40 lower-case hex digits, then the closing NUL.
inline constexpr std::size_t kCommitBytes = 40;
// The bytes read by each pread call.
inline constexpr std::size_t kChunkBytes = 65536;
// The longest whole literal: the prefix, the version, the space, the commit and the closing NUL.
inline constexpr std::size_t kLiteralBytes = kPrefixBytes + kMaxVersionBytes + 1 + kCommitBytes + 1;

// The characters a version is written with (build_identity.h: [0-9A-Za-z.+-]).
inline bool VersionByte(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '+' || c == '-';
}

// The characters a commit is written with (build_identity.h: [0-9a-f]).
inline bool CommitByte(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// The version of the first whole literal in bytes[0..size): the prefix, a version, a space, a 40-digit commit and the closing
// NUL, all inside the bytes (the pattern in build_identity.h). The prefix alone is not enough: a file built from sources that
// include this header can carry a copy of the prefix in its read-only data, and the commit digits and the closing NUL after the
// version are what tell a build's literal from that copy. Empty when there is none.
inline std::string VersionIn(const char* bytes, std::size_t size) {
  for (std::size_t at = 0; at + kPrefixBytes <= size; ++at) {
    if (std::memcmp(bytes + at, kPrefix, kPrefixBytes) != 0) continue;
    const std::size_t start = at + kPrefixBytes;
    std::size_t end = start;
    while (end < size && end - start <= kMaxVersionBytes && VersionByte(bytes[end])) ++end;
    const std::size_t length = end - start;
    if (length == 0 || length > kMaxVersionBytes || end >= size || bytes[end] != ' ') continue;
    const std::size_t commit = end + 1;
    if (size - commit < kCommitBytes + 1 || bytes[commit + kCommitBytes] != '\0') continue;
    bool whole = true;
    for (std::size_t i = 0; i < kCommitBytes; ++i) {
      if (!CommitByte(bytes[commit + i])) {
        whole = false;
        break;
      }
    }
    if (!whole) continue;
    return std::string(bytes + start, length);
  }
  return std::string();
}

// The version of the literal in the file `fd` names, which is `size` bytes long. It is read from offset 0 in kChunkBytes
// pieces; the last kLiteralBytes - 1 bytes of each piece are kept for the next, so a literal across two reads is found whole.
// Empty when the file has none or a read fails.
inline std::string VersionOfFile(int fd, std::uint64_t size) {
  char buffer[kLiteralBytes + kChunkBytes];
  std::size_t kept = 0;
  std::uint64_t offset = 0;
  while (offset < size) {
    const ssize_t got = ::pread(fd, buffer + kept, kChunkBytes, static_cast<off_t>(offset));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return std::string();
    const std::size_t total = kept + static_cast<std::size_t>(got);
    std::string version = VersionIn(buffer, total);
    if (!version.empty()) return version;
    kept = total < kLiteralBytes - 1 ? total : kLiteralBytes - 1;
    std::memmove(buffer, buffer + total - kept, kept);
    offset += static_cast<std::uint64_t>(got);
  }
  return std::string();
}

}  // namespace nevr_build_literal

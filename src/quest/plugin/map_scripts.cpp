// script_redirect::map_scripts::Register (script_redirect.h): the file checks before a map script is held. Kept out of
// script_redirect.cpp so the stock redirect's host tests and the dlopen handler's objects link without the ELF code. The
// decisions both platforms make (the arguments, the size, the digest and the claim) are the shared ones (core/map_script_check.h).
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/map_script_check.h"
#include "core/script_image_rules.h"
#include "hook_log.h"
#include "quest/data/sha256.h"
#include "quest/plugin/script_redirect.h"

namespace nevr_quest::script_redirect::map_scripts {

namespace {

// The widths the lines and paths use, named so no literal sits in a call.
constexpr std::size_t kHexStringBytes = 2 + 16 + 1;  // "0x", sixteen hex digits and the NUL (sentinel::HexString)
constexpr std::size_t kPathBytes = 1024;             // the library's full path
constexpr std::size_t kDirBytes = 512;               // the directory the sentinel was loaded from

std::uint64_t g_byteLimit = 0;  // set once by the channel (SetByteLimit) before any Register; the device's share

// The whole regular file at `path`, or a refusal token: its size must pass the bound and the pinned size (0: none pins it).
const char* ReadScript(const char* path, std::vector<std::uint8_t>* bytes, std::uint64_t pinned_bytes) {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? "missing" : "unreadable";
  struct stat st {};
  const char* reason = nullptr;
  if (::fstat(fd, &st) != 0) {
    reason = "unreadable";
  } else if (!S_ISREG(st.st_mode)) {
    reason = "missing";
  } else if (const char* size_reason =
                 nevr_map_script::SizeRefusal(static_cast<std::uint64_t>(st.st_size), pinned_bytes, g_byteLimit)) {
    reason = size_reason;
  } else {
    bytes->resize(static_cast<std::size_t>(st.st_size));
    std::size_t done = 0;
    while (done < bytes->size()) {
      const ssize_t got = ::read(fd, bytes->data() + done, bytes->size() - done);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) {
        reason = "unreadable";
        break;
      }
      done += static_cast<std::size_t>(got);
    }
  }
  ::close(fd);
  return reason;
}

// One map_script record per Register: Info when the script is held, Warning with the token when it is refused.
// KNOWN-WRONG #TBD: the Quest records are sentinel "map_script" records, not the [NEVR.MAPS] tag the PC provider logs under
// (docs/standards/logging.md, the [NEVR.MAPS] row); the Quest sentinel has no tag table row of its own for them.
const char* Outcome(const char* owner, std::uint64_t hash, const char* reason, const std::string& detail) {
  char hex[kHexStringBytes];
  if (reason == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "map_script",
                        {{"op", "register"}, {"result", "held"}, {"owner", owner}, {"hash", sentinel::HexString(hex, hash)},
                         {"route", "apk"}, {"held", HeldCount()}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "map_script",
                        {{"op", "register"}, {"result", "refused"}, {"owner", owner != nullptr ? owner : "(null)"},
                         {"hash", sentinel::HexString(hex, hash)}, {"reason", reason}, {"detail", detail.c_str()},
                         {"consequence", "the map's set may not carry this script; the map is not mounted with it"}});
  }
  return reason;
}

}  // namespace

void SetByteLimit(std::uint64_t byte_limit) noexcept { g_byteLimit = byte_limit; }

const char* RegisterIn(const char* libraryDir, const char* owner, std::uint64_t hash, const char* sha256,
                       std::uint64_t pinned_bytes) {
  const std::string ownerText = owner != nullptr ? owner : "";
  const std::string digest = sha256 != nullptr ? sha256 : "";
  if (g_byteLimit == 0) return Outcome(owner, hash, "not_configured", "no device fact gives the byte limit");
  if (const char* reason = nevr_map_script::ArgumentRefusal(ownerText, hash, digest)) return Outcome(owner, hash, reason, "");
  if (libraryDir == nullptr) return Outcome(owner, hash, "no_library_dir", "");
  char path[kPathBytes];
  const int written = std::snprintf(path, sizeof path, "%s/lib%016llx.so", libraryDir, static_cast<unsigned long long>(hash));
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof path) return Outcome(owner, hash, "no_library_dir", "path_too_long");
  std::vector<std::uint8_t> bytes;
  if (const char* reason = ReadScript(path, &bytes, pinned_bytes)) return Outcome(owner, hash, reason, path);
  const std::string actual = data::Sha256Hex(bytes.data(), bytes.size());
  if (const char* reason = nevr_map_script::DigestRefusal(actual, digest)) return Outcome(owner, hash, reason, actual);
  const nevr_script_image::ImageVerdict verdict = nevr_script_image::CheckElfScript(bytes.data(), bytes.size());
  if (!verdict.ok()) return Outcome(owner, hash, nevr_map_script::ImageRefusalToken(verdict.reason), verdict.detail);
  return Outcome(owner, hash, Admit(owner, hash), "");
}

const char* Register(const char* owner, std::uint64_t hash, const char* sha256, std::uint64_t pinned_bytes) {
  char dir[kDirBytes];
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&Register), &info) == 0 || info.dli_fname == nullptr) {
    return Outcome(owner, hash, "no_library_dir", "dladdr_failed");
  }
  if (const char* reason = LibraryDirectoryOf(info.dli_fname, dir, sizeof dir)) {
    return Outcome(owner, hash, "no_library_dir", reason);
  }
  return RegisterIn(dir, owner, hash, sha256, pinned_bytes);
}

}  // namespace nevr_quest::script_redirect::map_scripts

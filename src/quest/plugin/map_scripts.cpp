// script_redirect::map_scripts::Register (script_redirect.h): the file checks before a map script is held. Kept out of
// script_redirect.cpp so the stock redirect's host tests and the dlopen handler's objects link without the hash and ELF code.
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/script_image_rules.h"
#include "hook_log.h"
#include "quest/data/sha256.h"
#include "quest/plugin/script_redirect.h"

namespace nevr_quest::script_redirect::map_scripts {

namespace {

constexpr std::uint64_t kMaxScriptBytes = 64ull << 20;  // the PC provider's bad_size bound
constexpr char kOwnerPrefix[] = "map:";

bool IsLowerHex64(const char* text) {
  if (text == nullptr || std::strlen(text) != 64) return false;
  for (const char* c = text; *c != '\0'; ++c) {
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return false;
  }
  return true;
}

bool ValidOwner(const char* owner) {
  if (owner == nullptr || std::strncmp(owner, kOwnerPrefix, sizeof kOwnerPrefix - 1) != 0) return false;
  const std::size_t length = std::strlen(owner);
  return length > sizeof kOwnerPrefix - 1 && length < kOwnerCapacity;
}

// The whole regular file at `path`, or a refusal token.
const char* ReadScript(const char* path, std::vector<std::uint8_t>* bytes) {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? "missing" : "unreadable";
  struct stat st {};
  const char* reason = nullptr;
  if (::fstat(fd, &st) != 0) {
    reason = "unreadable";
  } else if (!S_ISREG(st.st_mode)) {
    reason = "missing";
  } else if (st.st_size <= 0 || static_cast<std::uint64_t>(st.st_size) > kMaxScriptBytes) {
    reason = "bad_size";
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

const char* Outcome(const char* owner, std::uint64_t hash, const char* reason, const std::string& detail) {
  char hex[19];
  if (reason == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "map_script",
                        {{"op", "register"}, {"result", "held"}, {"owner", owner}, {"hash", sentinel::HexString(hex, hash)},
                         {"route", "apk"}, {"held", HeldCount()}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "map_script",
                        {{"op", "register"}, {"result", "refused"}, {"owner", owner != nullptr ? owner : "(null)"},
                         {"hash", sentinel::HexString(hex, hash)}, {"reason", reason}, {"detail", detail.c_str()},
                         {"consequence", "the map's set may not carry this script; the map is not mounted with it"}});
  }
  return reason;
}

// The verdict's token as a string with static storage: Register's answer outlives the verdict.
const char* ElfToken(const std::string& reason) {
  static constexpr const char* kTokens[] = {
      "elf_truncated", "elf_not_elf",    "elf_not_64",    "elf_not_le",    "elf_not_dyn",   "elf_not_aarch64",
      "elf_segment_wx", "elf_no_dynamic", "elf_textrel",   "elf_no_symbols", "elf_needed",   "elf_init",
      "elf_no_export",  "elf_export_not_code"};
  for (const char* token : kTokens) {
    if (reason == token) return token;
  }
  return "elf_rejected";
}

}  // namespace

const char* RegisterIn(const char* libraryDir, const char* owner, std::uint64_t hash, const char* sha256) {
  if (!ValidOwner(owner)) return Outcome(owner, hash, "bad_owner", "");
  if (!IsLowerHex64(sha256)) return Outcome(owner, hash, "bad_sha256", "");
  if (hash == 0) return Outcome(owner, hash, "bad_hash", "");
  if (libraryDir == nullptr) return Outcome(owner, hash, "no_library_dir", "");
  char path[1024];
  const int written = std::snprintf(path, sizeof path, "%s/lib%016llx.so", libraryDir, static_cast<unsigned long long>(hash));
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof path) return Outcome(owner, hash, "no_library_dir", "path_too_long");
  std::vector<std::uint8_t> bytes;
  if (const char* reason = ReadScript(path, &bytes)) return Outcome(owner, hash, reason, path);
  const std::string actual = data::Sha256Hex(bytes.data(), bytes.size());
  if (actual != sha256) return Outcome(owner, hash, "sha_mismatch", actual);
  const nevr_script_image::ImageVerdict verdict = nevr_script_image::CheckElfScript(bytes.data(), bytes.size());
  if (!verdict.ok()) return Outcome(owner, hash, ElfToken(verdict.reason), verdict.detail);
  return Outcome(owner, hash, Admit(owner, hash), "");
}

const char* Register(const char* owner, std::uint64_t hash, const char* sha256) {
  char dir[512];
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&Register), &info) == 0 || info.dli_fname == nullptr) {
    return Outcome(owner, hash, "no_library_dir", "dladdr_failed");
  }
  if (const char* reason = LibraryDirectoryOf(info.dli_fname, dir, sizeof dir)) {
    return Outcome(owner, hash, "no_library_dir", reason);
  }
  return RegisterIn(dir, owner, hash, sha256);
}

}  // namespace nevr_quest::script_redirect::map_scripts

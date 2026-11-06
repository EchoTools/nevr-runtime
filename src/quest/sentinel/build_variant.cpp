#include "quest/sentinel/build_variant.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace nevr_quest::build_variant {

namespace {

constexpr std::size_t kPathBytes = 4096;

Stamp Make(Variant variant, StampSource source, const char* why) noexcept {
  Stamp stamp;
  stamp.variant = variant;
  stamp.source = source;
  stamp.why = why;
  return stamp;
}

struct Word {
  const char* text;
  Variant variant;
};
constexpr Word kWords[] = {{"q15t", Variant::kQ15t}, {"combat", Variant::kCombat}, {"perf-probe", Variant::kPerfProbe}};

}  // namespace

const char* VariantName(Variant variant) noexcept {
  switch (variant) {
    case Variant::kNone: return "none";
    case Variant::kQ15t: return "q15t";
    case Variant::kCombat: return "combat";
    case Variant::kPerfProbe: return "perf-probe";
  }
  return "none";
}

const char* SourceName(StampSource source) noexcept {
  switch (source) {
    case StampSource::kStamp: return "stamp";
    case StampSource::kAbsent: return "absent";
    case StampSource::kUnreadable: return "unreadable";
    case StampSource::kInvalid: return "invalid";
  }
  return "invalid";
}

Stamp ParseStamp(const char* bytes, std::size_t size) noexcept {
  if (size == 0) return Make(Variant::kNone, StampSource::kStamp, "");
  if (size > kMaxStampBytes) return Make(Variant::kNone, StampSource::kInvalid, "too_long");
  if (bytes != nullptr) {
    for (const Word& word : kWords) {
      if (std::strlen(word.text) == size && std::memcmp(word.text, bytes, size) == 0) {
        return Make(word.variant, StampSource::kStamp, "");
      }
    }
  }
  return Make(Variant::kNone, StampSource::kInvalid, "unknown_word");
}

Stamp ReadStamp(const char* directory) noexcept {
  if (directory == nullptr || directory[0] == '\0') return Make(Variant::kNone, StampSource::kUnreadable, "no_library_dir");
  char path[kPathBytes];
  const int written = std::snprintf(path, sizeof(path), "%s/%s", directory, kStampFile);
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof(path)) {
    return Make(Variant::kNone, StampSource::kUnreadable, "open_failed");
  }
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT) return Make(Variant::kNone, StampSource::kAbsent, "no_file");
    return Make(Variant::kNone, StampSource::kUnreadable, errno == ELOOP ? "symlink" : "open_failed");  // O_NOFOLLOW makes a link ELOOP
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return Make(Variant::kNone, StampSource::kUnreadable, "not_regular_file");
  }
  if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > kMaxStampBytes) {
    ::close(fd);
    return Make(Variant::kNone, StampSource::kInvalid, "too_long");
  }
  char bytes[kMaxStampBytes];
  std::size_t have = 0;
  for (;;) {
    const ssize_t got = ::read(fd, bytes + have, sizeof(bytes) - have);
    if (got < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return Make(Variant::kNone, StampSource::kUnreadable, "read_failed");
    }
    if (got == 0) break;
    have += static_cast<std::size_t>(got);
    if (have == sizeof(bytes)) break;
  }
  ::close(fd);
  return ParseStamp(bytes, have);
}

const char* LibraryDirectoryOf(const char* path, char* dir, std::size_t cap) noexcept {
  if (path == nullptr || dir == nullptr || cap == 0) return "no_library_dir";
  if (std::strstr(path, "!/") != nullptr) return "mapped_from_apk";
  const char* slash = std::strrchr(path, '/');
  if (slash == nullptr) return "no_library_dir";
  const std::size_t length = static_cast<std::size_t>(slash - path);
  if (length + 1 > cap) return "no_library_dir";
  std::memcpy(dir, path, length);
  dir[length] = '\0';
  return nullptr;
}

Stamp ReadOwnStamp() noexcept {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&ReadOwnStamp), &info) == 0 || info.dli_fname == nullptr) {
    return Make(Variant::kNone, StampSource::kUnreadable, "no_library_dir");
  }
  char directory[kPathBytes];
  if (const char* const why = LibraryDirectoryOf(info.dli_fname, directory, sizeof(directory))) {
    return Make(Variant::kNone, StampSource::kUnreadable, why);
  }
  return ReadStamp(directory);
}

}  // namespace nevr_quest::build_variant

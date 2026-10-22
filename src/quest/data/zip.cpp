#include "quest/data/zip.h"

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nevr_quest::data {

namespace {

constexpr std::uint32_t kEndSig = 0x06054b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kZip64LocatorSig = 0x07064b50;
constexpr std::size_t kEndSize = 22;
constexpr std::size_t kCentralFixed = 46;
constexpr std::size_t kLocalFixed = 30;

std::uint16_t Rd16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t Rd32(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// RAII file descriptor.
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  int fd_;
};

bool PreadAll(int fd, void* buffer, std::size_t length, std::uint64_t offset) {
  unsigned char* p = static_cast<unsigned char*>(buffer);
  while (length != 0) {
    const ssize_t n = ::pread(fd, p, length, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    offset += static_cast<std::uint64_t>(n);
    length -= static_cast<std::size_t>(n);
  }
  return true;
}

bool WriteAll(int fd, const void* buffer, std::size_t length) {
  const unsigned char* p = static_cast<const unsigned char*>(buffer);
  while (length != 0) {
    const ssize_t n = ::write(fd, p, length);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    length -= static_cast<std::size_t>(n);
  }
  return true;
}

// The detail reaches a log line: a control byte in an entry name (the name is untrusted) becomes '?'.
void SetDetail(std::string* detail, const std::string& text) {
  if (detail == nullptr) return;
  *detail = text;
  for (char& c : *detail) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) c = '?';
  }
}

// Names that differ only in ASCII case are the same name on a case-insensitive filesystem.
std::string FoldCase(std::string name) {
  for (char& c : name) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return name;
}

}  // namespace

bool MakeDirsBelow(const std::string& base, const std::string& rel) {
  std::string path = base;
  std::size_t start = 0;
  while (start <= rel.size() && !rel.empty()) {
    const std::size_t slash = rel.find('/', start);
    const std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    path += "/" + part;
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;  // a link or a file is not a directory
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

namespace {

std::string StripSlash(const std::string& name) {
  return (!name.empty() && name.back() == '/') ? name.substr(0, name.size() - 1) : name;
}

}  // namespace

const char* ZipStatusName(ZipStatus status) noexcept {
  switch (status) {
    case ZipStatus::kOk: return "ok";
    case ZipStatus::kOpenFailed: return "open_failed";
    case ZipStatus::kNoEndRecord: return "no_end_record";
    case ZipStatus::kZip64: return "zip64";
    case ZipStatus::kMultiDisk: return "multi_disk";
    case ZipStatus::kBadCentralDirectory: return "bad_central_directory";
    case ZipStatus::kEncrypted: return "encrypted";
    case ZipStatus::kUnsupportedMethod: return "unsupported_method";
    case ZipStatus::kSpecialFile: return "special_file";
    case ZipStatus::kTooManyEntries: return "too_many_entries";
    case ZipStatus::kBadEntryPath: return "bad_entry_path";
    case ZipStatus::kConflict: return "conflict";
    case ZipStatus::kOffsetOutOfRange: return "offset_out_of_range";
    case ZipStatus::kSizeLimit: return "size_limit";
    case ZipStatus::kBadLocalHeader: return "bad_local_header";
    case ZipStatus::kDestExists: return "dest_exists";
    case ZipStatus::kWriteFailed: return "write_failed";
    case ZipStatus::kReadFailed: return "read_failed";
    case ZipStatus::kInflateFailed: return "inflate_failed";
    case ZipStatus::kSizeMismatch: return "size_mismatch";
    case ZipStatus::kCrcMismatch: return "crc_mismatch";
  }
  return "unknown";
}

bool SafeEntryName(const std::string& name) noexcept {
  if (name.empty() || name.size() > 512) return false;
  if (name.front() == '/') return false;
  std::size_t components = 0;
  std::size_t start = 0;
  const std::string body = StripSlash(name);
  if (body.empty()) return false;
  while (true) {
    const std::size_t slash = body.find('/', start);
    const std::string part = body.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part.empty() || part == "." || part == ".." || part.size() > 255) return false;
    for (const char c : part) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (c == '\\' || u < 0x20 || u == 0x7f) return false;
    }
    if (part.size() >= 2 && part[1] == ':') return false;  // a drive letter
    if (++components > 16) return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

ZipStatus ReadZipListing(const std::string& path, const ZipLimits& limits, std::vector<ZipEntry>* entries,
                         std::string* detail) {
  entries->clear();
  Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) {
    SetDetail(detail, path);
    return ZipStatus::kOpenFailed;
  }
  struct stat st;
  if (::fstat(fd.get(), &st) != 0 || st.st_size < static_cast<off_t>(kEndSize)) return ZipStatus::kNoEndRecord;
  const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);

  // The end record: the last 22 bytes plus a comment of at most 65535.
  const std::uint64_t tail_size = std::min<std::uint64_t>(file_size, kEndSize + 65535);
  std::vector<unsigned char> tail(static_cast<std::size_t>(tail_size));
  if (!PreadAll(fd.get(), tail.data(), tail.size(), file_size - tail_size)) return ZipStatus::kReadFailed;
  std::size_t end_at = tail.size();
  for (std::size_t i = tail.size() - kEndSize + 1; i-- > 0;) {
    if (Rd32(&tail[i]) == kEndSig && i + kEndSize + Rd16(&tail[i + 20]) == tail.size()) {
      end_at = i;
      break;
    }
  }
  if (end_at == tail.size()) return ZipStatus::kNoEndRecord;
  const unsigned char* end = &tail[end_at];
  const std::uint64_t end_offset = file_size - tail_size + end_at;
  if (end_at >= 20 && Rd32(&tail[end_at - 20]) == kZip64LocatorSig) return ZipStatus::kZip64;
  const std::uint16_t disk = Rd16(end + 4), cd_disk = Rd16(end + 6), on_disk = Rd16(end + 8), total = Rd16(end + 10);
  const std::uint32_t cd_size = Rd32(end + 12), cd_offset = Rd32(end + 16);
  if (total == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) return ZipStatus::kZip64;
  if (disk != 0 || cd_disk != 0 || on_disk != total) return ZipStatus::kMultiDisk;
  if (total > limits.max_entries) return ZipStatus::kTooManyEntries;
  if (static_cast<std::uint64_t>(cd_offset) + cd_size > end_offset) return ZipStatus::kBadCentralDirectory;

  std::vector<unsigned char> directory(cd_size);
  if (cd_size != 0 && !PreadAll(fd.get(), directory.data(), directory.size(), cd_offset)) return ZipStatus::kReadFailed;
  std::size_t p = 0;
  std::uint64_t sum = 0;
  std::set<std::string> names;
  for (std::uint16_t i = 0; i < total; ++i) {
    if (p + kCentralFixed > directory.size() || Rd32(&directory[p]) != kCentralSig) {
      return ZipStatus::kBadCentralDirectory;
    }
    const unsigned char* h = &directory[p];
    const std::uint16_t made_by = Rd16(h + 4), flags = Rd16(h + 8), method = Rd16(h + 10);
    const std::uint32_t crc = Rd32(h + 16), csize = Rd32(h + 20), usize = Rd32(h + 24);
    const std::uint16_t name_len = Rd16(h + 28), extra_len = Rd16(h + 30), comment_len = Rd16(h + 32);
    const std::uint32_t external = Rd32(h + 38), local_offset = Rd32(h + 42);
    if (p + kCentralFixed + name_len + extra_len + comment_len > directory.size()) return ZipStatus::kBadCentralDirectory;
    ZipEntry e;
    e.name.assign(reinterpret_cast<const char*>(h + kCentralFixed), name_len);
    p += kCentralFixed + name_len + extra_len + comment_len;
    if ((flags & 0x1) != 0 || (flags & 0x40) != 0 || (flags & 0x2000) != 0) {
      SetDetail(detail, e.name);
      return ZipStatus::kEncrypted;
    }
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local_offset == 0xFFFFFFFFu) return ZipStatus::kZip64;
    if (method != 0 && method != 8) {
      SetDetail(detail, e.name);
      return ZipStatus::kUnsupportedMethod;
    }
    if (method == 0 && csize != usize) return ZipStatus::kBadCentralDirectory;
    if ((made_by >> 8) == 3) {  // created on Unix: the mode is in the high 16 bits of the external attributes
      const std::uint32_t mode = external >> 16;
      if (mode != 0 && (mode & S_IFMT) != S_IFREG && (mode & S_IFMT) != S_IFDIR) {
        SetDetail(detail, e.name);
        return ZipStatus::kSpecialFile;
      }
    }
    if (!SafeEntryName(e.name)) {
      SetDetail(detail, e.name);
      return ZipStatus::kBadEntryPath;
    }
    e.method = method;
    e.crc32 = crc;
    e.compressed_size = csize;
    e.size = usize;
    e.local_header_offset = local_offset;
    if (e.IsDirectory() && (csize != 0 || usize != 0)) return ZipStatus::kBadCentralDirectory;
    if (usize > limits.max_entry_bytes) {
      SetDetail(detail, e.name);
      return ZipStatus::kSizeLimit;
    }
    sum += usize;
    if (sum > limits.max_total_bytes) {
      SetDetail(detail, "total");
      return ZipStatus::kSizeLimit;
    }
    if (static_cast<std::uint64_t>(local_offset) + kLocalFixed + csize > cd_offset) {
      SetDetail(detail, e.name);
      return ZipStatus::kOffsetOutOfRange;
    }
    if (!names.insert(FoldCase(StripSlash(e.name))).second) {
      SetDetail(detail, e.name);
      return ZipStatus::kConflict;
    }
    entries->push_back(std::move(e));
  }
  if (p != directory.size()) return ZipStatus::kBadCentralDirectory;
  // A file that is also a directory of another entry.
  for (const ZipEntry& e : *entries) {
    if (e.IsDirectory()) continue;
    const std::string prefix = FoldCase(e.name) + "/";
    const auto it = names.lower_bound(prefix);
    if (it != names.end() && it->compare(0, prefix.size(), prefix) == 0) {
      SetDetail(detail, e.name);
      return ZipStatus::kConflict;
    }
  }
  return ZipStatus::kOk;
}

namespace {

// The local header agrees with the central directory: the same name and method, no encryption flag, and, when the entry
// has no data descriptor (flag bit 3), the same CRC and sizes.
bool LocalHeaderMatches(int zip_fd, const ZipEntry& e, const unsigned char* local) {
  const std::uint16_t flags = Rd16(local + 6), method = Rd16(local + 8), name_len = Rd16(local + 26);
  if (method != e.method || (flags & (0x1 | 0x40 | 0x2000)) != 0) return false;
  if ((flags & 0x8) == 0 && (Rd32(local + 14) != e.crc32 || Rd32(local + 18) != e.compressed_size ||
                             Rd32(local + 22) != e.size)) {
    return false;
  }
  std::string name(name_len, '\0');
  return PreadAll(zip_fd, name.data(), name.size(), e.local_header_offset + kLocalFixed) && name == e.name;
}

// Writes one entry's data to `out_fd`: copies a stored entry or inflates a deflated one, enforcing the sizes and CRC the
// central directory recorded.
ZipStatus ExtractData(int zip_fd, const ZipEntry& e, std::uint64_t data_offset, int out_fd) {
  std::vector<unsigned char> in(64 * 1024), out(256 * 1024);
  std::uint64_t consumed = 0, produced = 0;
  std::uint32_t crc = static_cast<std::uint32_t>(::crc32(0L, Z_NULL, 0));
  z_stream zs;
  std::memset(&zs, 0, sizeof(zs));
  const bool deflated = e.method == 8;
  if (deflated && ::inflateInit2(&zs, -MAX_WBITS) != Z_OK) return ZipStatus::kInflateFailed;
  ZipStatus status = ZipStatus::kOk;
  bool stream_end = !deflated;
  while (consumed < e.compressed_size && status == ZipStatus::kOk) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(in.size(), e.compressed_size - consumed));
    if (!PreadAll(zip_fd, in.data(), want, data_offset + consumed)) {
      status = ZipStatus::kReadFailed;
      break;
    }
    consumed += want;
    if (!deflated) {
      produced += want;
      crc = static_cast<std::uint32_t>(::crc32(crc, in.data(), static_cast<uInt>(want)));
      if (!WriteAll(out_fd, in.data(), want)) status = ZipStatus::kWriteFailed;
      continue;
    }
    zs.next_in = in.data();
    zs.avail_in = static_cast<uInt>(want);
    while (zs.avail_in != 0 && status == ZipStatus::kOk) {
      zs.next_out = out.data();
      zs.avail_out = static_cast<uInt>(out.size());
      const int rc = ::inflate(&zs, Z_NO_FLUSH);
      const std::size_t made = out.size() - zs.avail_out;
      if (made != 0) {
        produced += made;
        if (produced > e.size) {  // a stream that expands past its recorded size
          status = ZipStatus::kSizeMismatch;
          break;
        }
        crc = static_cast<std::uint32_t>(::crc32(crc, out.data(), static_cast<uInt>(made)));
        if (!WriteAll(out_fd, out.data(), made)) {
          status = ZipStatus::kWriteFailed;
          break;
        }
      }
      if (rc == Z_STREAM_END) {
        stream_end = true;
        if (zs.avail_in != 0) status = ZipStatus::kSizeMismatch;  // bytes after the end of the stream
        break;
      }
      if (rc != Z_OK) {
        status = ZipStatus::kInflateFailed;
        break;
      }
    }
  }
  if (deflated) ::inflateEnd(&zs);
  if (status != ZipStatus::kOk) return status;
  if (!stream_end) return ZipStatus::kInflateFailed;  // the data ended before the stream did
  if (produced != e.size) return ZipStatus::kSizeMismatch;
  if (crc != e.crc32) return ZipStatus::kCrcMismatch;
  if (::fsync(out_fd) != 0) return ZipStatus::kWriteFailed;
  return ZipStatus::kOk;
}

}  // namespace

ZipStatus ExtractZip(const std::string& zip_path, const std::string& dest_dir, const std::string& strip_prefix,
                     const ZipLimits& limits, std::uint64_t* bytes_written, std::string* detail) {
  if (bytes_written != nullptr) *bytes_written = 0;
  std::vector<ZipEntry> entries;
  const ZipStatus listed = ReadZipListing(zip_path, limits, &entries, detail);
  if (listed != ZipStatus::kOk) return listed;
  // Every name must carry the prefix, before anything is written.
  for (const ZipEntry& e : entries) {
    if (e.name.compare(0, strip_prefix.size(), strip_prefix) != 0) {
      SetDetail(detail, e.name);
      return ZipStatus::kBadEntryPath;
    }
  }
  Fd zip(::open(zip_path.c_str(), O_RDONLY | O_CLOEXEC));
  if (zip.get() < 0) return ZipStatus::kOpenFailed;
  if (::mkdir(dest_dir.c_str(), 0755) != 0) {
    SetDetail(detail, dest_dir);
    return errno == EEXIST ? ZipStatus::kDestExists : ZipStatus::kWriteFailed;
  }
  std::uint64_t written = 0;
  for (const ZipEntry& e : entries) {
    const std::string rel = StripSlash(e.name.substr(strip_prefix.size()));
    if (rel.empty()) continue;  // the prefix directory itself is dest_dir
    if (e.IsDirectory()) {
      if (!MakeDirsBelow(dest_dir, rel)) {
        SetDetail(detail, e.name);
        return ZipStatus::kWriteFailed;
      }
      continue;
    }
    const std::size_t slash = rel.rfind('/');
    if (slash != std::string::npos && !MakeDirsBelow(dest_dir, rel.substr(0, slash))) {
      SetDetail(detail, e.name);
      return ZipStatus::kWriteFailed;
    }
    unsigned char local[kLocalFixed];
    if (!PreadAll(zip.get(), local, sizeof(local), e.local_header_offset) || Rd32(local) != kLocalSig) {
      SetDetail(detail, e.name);
      return ZipStatus::kBadLocalHeader;
    }
    if (!LocalHeaderMatches(zip.get(), e, local)) {
      SetDetail(detail, e.name);
      return ZipStatus::kBadLocalHeader;
    }
    const std::uint64_t data_offset = e.local_header_offset + kLocalFixed + Rd16(local + 26) + Rd16(local + 28);
    struct stat zst;
    if (::fstat(zip.get(), &zst) != 0 || data_offset + e.compressed_size > static_cast<std::uint64_t>(zst.st_size)) {
      SetDetail(detail, e.name);
      return ZipStatus::kOffsetOutOfRange;
    }
    const std::string target = dest_dir + "/" + rel;
    Fd out(::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
    if (out.get() < 0) {
      SetDetail(detail, e.name);
      return ZipStatus::kWriteFailed;
    }
    const ZipStatus status = ExtractData(zip.get(), e, data_offset, out.get());
    if (status != ZipStatus::kOk) {
      SetDetail(detail, e.name);
      return status;
    }
    written += e.size;
    if (bytes_written != nullptr) *bytes_written = written;
  }
  return ZipStatus::kOk;
}

}  // namespace nevr_quest::data

#include "quest/cargo/stock_source_posix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "quest/cargo/posix_map_fs.h"
#include "quest/data/plan.h"
#include "quest/data/zip.h"

namespace nevr_quest::maps {
namespace {

using nevr_maps::FileStamp;
using nevr_maps::StockStamp;

// docs/reference/package-manifest-format.md, the same layout stock_source_win.cpp reads on the PC: a 24-byte container around one
// zstd frame; the body's counts and arrays.
constexpr std::size_t kContainerBytes = 24;
constexpr std::uint32_t kContainerVersion = 0x10;
constexpr std::uint64_t kArraysAt = 0xc0, kResourceBytes = 0x20, kFileBytes = 0x28, kChunkBytes = 0x10;
constexpr std::size_t kResourceCountAt = 0x38, kFileCountAt = 0x78, kChunkCountAt = 0xb8;
constexpr std::uint64_t kMaxManifestFileBytes = 64ull << 20, kMaxManifestBodyBytes = 256ull << 20;
constexpr std::size_t kReadStep = 1u << 20;
constexpr const char* kApkLibDir = "lib/arm64-v8a/";
constexpr const char* kStockSuffix = ".stock";

bool Inside(std::uint64_t offset, std::uint64_t length, std::uint64_t total) {
  return offset <= total && length <= total - offset;
}

std::uint64_t Load(const std::uint8_t* p, unsigned width) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) value |= std::uint64_t{p[i]} << (8 * i);
  return value;
}

std::string Hex16(std::uint64_t value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

bool IsManifestName(const std::string& name) {
  return name.size() == 16 &&
         std::all_of(name.begin(), name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool IsPlainName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos;
}

std::vector<std::string> ListNames(const std::string& dir) {
  std::vector<std::string> names;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return names;
  while (const dirent* entry = ::readdir(d)) names.emplace_back(entry->d_name);
  ::closedir(d);
  std::sort(names.begin(), names.end());
  return names;
}

bool IsDir(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

class File {
 public:
  File() = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File() {
    if (fd_ >= 0) ::close(fd_);
  }

  bool Open(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    return fd_ >= 0;
  }

  bool Stamp(FileStamp* out) {
    PosixMapFs fs;
    return fs.StampOf(fd_, out);
  }

  bool Read(std::uint64_t offset, void* buffer, std::size_t length) const {
    auto* out = static_cast<std::uint8_t*>(buffer);
    while (length > 0) {
      const ssize_t got = ::pread(fd_, out, std::min(length, kReadStep), static_cast<off_t>(offset));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) return false;
      out += got;
      offset += static_cast<std::uint64_t>(got);
      length -= static_cast<std::size_t>(got);
    }
    return true;
  }

 private:
  int fd_ = -1;
};

bool OpenStamped(const std::string& path, File* file, FileStamp* stamp, std::vector<StockStamp>* read_from) {
  if (!file->Open(path) || !file->Stamp(stamp)) return false;
  read_from->push_back({path, *stamp});
  return true;
}

struct Resource {
  std::uint64_t type = 0, name = 0;
  std::uint32_t chunk = 0, offset = 0, size = 0;
};
struct Chunk {
  std::uint32_t package = 0, offset = 0, compressed = 0, decompressed = 0;
};
struct Manifest {
  FileStamp stamp;
  bool ok = false;
  std::uint32_t packages = 0;
  std::vector<Resource> resources;  // sorted by (type, name)
  std::vector<Chunk> chunks;
};

bool ResourceLess(const Resource& a, const Resource& b) { return a.type != b.type ? a.type < b.type : a.name < b.name; }

bool ParseManifest(const std::vector<std::uint8_t>& file, Manifest* out) {
  if (file.size() < kContainerBytes || std::memcmp(file.data(), "ZSTD", 4) != 0) return false;
  if (Load(file.data() + 4, 4) != kContainerVersion) return false;
  const std::uint64_t body_bytes = Load(file.data() + 8, 8), compressed = Load(file.data() + 16, 8);
  if (compressed > file.size() - kContainerBytes) return false;
  if (body_bytes < kArraysAt || body_bytes > kMaxManifestBodyBytes) return false;
  const unsigned long long declared = ZSTD_getFrameContentSize(file.data() + kContainerBytes, static_cast<std::size_t>(compressed));
  if (declared == ZSTD_CONTENTSIZE_ERROR || (declared != ZSTD_CONTENTSIZE_UNKNOWN && declared != body_bytes)) return false;
  std::vector<std::uint8_t> body(static_cast<std::size_t>(body_bytes));
  const std::size_t written =
      ZSTD_decompress(body.data(), body.size(), file.data() + kContainerBytes, static_cast<std::size_t>(compressed));
  if (ZSTD_isError(written) || written != body.size()) return false;

  const std::uint64_t resources = Load(body.data() + kResourceCountAt, 8);
  const std::uint64_t files = Load(body.data() + kFileCountAt, 8);
  const std::uint64_t chunks = Load(body.data() + kChunkCountAt, 8);
  std::uint64_t room = body_bytes - kArraysAt;
  if (resources > room / kResourceBytes) return false;
  room -= resources * kResourceBytes;
  if (files > room / kFileBytes) return false;
  room -= files * kFileBytes;
  if (chunks > room / kChunkBytes) return false;

  out->packages = static_cast<std::uint32_t>(Load(body.data(), 4));
  out->resources.resize(static_cast<std::size_t>(resources));
  for (std::size_t i = 0; i < out->resources.size(); ++i) {
    const std::uint8_t* at = body.data() + kArraysAt + i * kResourceBytes;
    Resource& r = out->resources[i];
    r.type = Load(at, 8);
    r.name = Load(at + 8, 8);
    r.chunk = static_cast<std::uint32_t>(Load(at + 0x10, 4));
    r.offset = static_cast<std::uint32_t>(Load(at + 0x14, 4));
    r.size = static_cast<std::uint32_t>(Load(at + 0x18, 4));
  }
  std::sort(out->resources.begin(), out->resources.end(), ResourceLess);
  const std::uint8_t* chunk_table = body.data() + kArraysAt + resources * kResourceBytes + files * kFileBytes;
  out->chunks.resize(static_cast<std::size_t>(chunks));
  for (std::size_t i = 0; i < out->chunks.size(); ++i) {
    const std::uint8_t* at = chunk_table + i * kChunkBytes;
    Chunk& c = out->chunks[i];
    c.package = static_cast<std::uint32_t>(Load(at, 4));
    c.offset = static_cast<std::uint32_t>(Load(at + 4, 4));
    c.compressed = static_cast<std::uint32_t>(Load(at + 8, 4));
    c.decompressed = static_cast<std::uint32_t>(Load(at + 12, 4));
  }
  out->ok = true;
  return true;
}

// Decompresses the one zstd frame of `compressed` bytes at `offset` into exactly `out->size()` bytes, kReadStep at a time.
bool DecompressFrame(const File& file, std::uint64_t offset, std::uint64_t compressed, std::vector<std::uint8_t>* out) {
  std::unique_ptr<ZSTD_DCtx, std::size_t (*)(ZSTD_DCtx*)> context(ZSTD_createDCtx(), ZSTD_freeDCtx);
  if (!context) return false;
  std::vector<std::uint8_t> step(static_cast<std::size_t>(std::min<std::uint64_t>(compressed, kReadStep)));
  ZSTD_outBuffer output = {out->data(), out->size(), 0};
  std::size_t last = 1;
  while (compressed > 0) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(compressed, step.size()));
    if (!file.Read(offset, step.data(), n)) return false;
    offset += n;
    compressed -= n;
    ZSTD_inBuffer input = {step.data(), n, 0};
    while (input.pos < input.size) {
      const std::size_t before = input.pos, filled = output.pos;
      last = ZSTD_decompressStream(context.get(), &output, &input);
      if (ZSTD_isError(last)) return false;
      if (input.pos == before && output.pos == filled) return false;  // the frame wants room the record does not have
    }
  }
  return last == 0 && output.pos == output.size;
}

class PosixStockSource : public nevr_maps::StockSource {
 public:
  PosixStockSource(DataRootFn data_root, std::string native_lib_dir, std::string apk_path)
      : data_root_(data_root), lib_dir_(std::move(native_lib_dir)), apk_(std::move(apk_path)) {}

  bool ReadResource(std::uint64_t type, std::uint64_t name, std::vector<std::uint8_t>* out,
                    std::vector<StockStamp>* read_from) override {
    std::lock_guard<std::mutex> lock(mutex_);
    out->clear();
    const char* root = data_root_ != nullptr ? data_root_() : nullptr;
    if (root == nullptr || *root == '\0') return false;
    const std::string data_dir = std::string(root) + "/_data";
    for (const std::string& folder : ListNames(data_dir)) {
      if (!IsPlainName(folder)) continue;
      const std::string android = data_dir + "/" + folder + "/rad15/android";
      const std::string manifests = android + "/manifests";
      if (!IsDir(manifests)) continue;
      for (const std::string& id : ManifestIds(manifests)) {
        const std::string stock = manifests + "/" + id + kStockSuffix;
        const Manifest* manifest = LoadManifest(IsFile(stock) ? stock : manifests + "/" + id, read_from);
        if (manifest == nullptr) continue;
        Resource key;
        key.type = type;
        key.name = name;
        const auto it = std::lower_bound(manifest->resources.begin(), manifest->resources.end(), key, ResourceLess);
        if (it == manifest->resources.end() || it->type != type || it->name != name) continue;
        return ReadFromPackage(android + "/packages/" + id, *manifest, *it, out, read_from);
      }
    }
    return false;
  }

  bool ReadScript(std::uint64_t hash, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) override {
    out->clear();
    const std::string file_name = "lib" + Hex16(hash) + ".so";
    if (!lib_dir_.empty()) {
      const std::string path = lib_dir_ + "/" + file_name;
      if (IsFile(path)) return ReadWhole(path, out, read_from);
    }
    return !apk_.empty() && ReadApkEntry(std::string(kApkLibDir) + file_name, out, read_from);
  }

  bool StampsCurrent(const std::vector<StockStamp>& stamps) override {
    for (const StockStamp& recorded : stamps) {
      File file;
      FileStamp now;
      if (!file.Open(recorded.path) || !file.Stamp(&now) || now != recorded.stamp) return false;
    }
    return true;
  }

 private:
  // The pinned manifest first, then every other manifest name in order; `.stock` copies are reached through their id.
  static std::vector<std::string> ManifestIds(const std::string& manifests_dir) {
    const std::string pinned_path = nevr_quest::data::kManifestPath;
    const std::string pinned = pinned_path.substr(pinned_path.rfind('/') + 1);
    std::vector<std::string> ids;
    for (std::string& name : ListNames(manifests_dir)) {
      if (name.size() == 16 + std::strlen(kStockSuffix) && name.compare(16, std::string::npos, kStockSuffix) == 0)
        name.resize(16);
      if (IsManifestName(name) && name != pinned) ids.push_back(std::move(name));
    }
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ids.insert(ids.begin(), pinned);
    return ids;
  }

  const Manifest* LoadManifest(const std::string& path, std::vector<StockStamp>* read_from) {
    File file;
    FileStamp stamp;
    if (!OpenStamped(path, &file, &stamp, read_from)) return nullptr;
    const auto found = manifests_.find(path);
    const bool fresh = found == manifests_.end() || found->second.stamp != stamp;
    Manifest& cached = manifests_[path];
    if (fresh) {
      cached = Manifest{};
      cached.stamp = stamp;
      std::vector<std::uint8_t> bytes;
      if (stamp.size <= kMaxManifestFileBytes) {
        bytes.resize(static_cast<std::size_t>(stamp.size));
        if (bytes.empty() || file.Read(0, bytes.data(), bytes.size())) ParseManifest(bytes, &cached);
      }
    }
    return cached.ok ? &cached : nullptr;
  }

  bool ReadFromPackage(const std::string& package_prefix, const Manifest& manifest, const Resource& resource,
                       std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) {
    if (resource.chunk >= manifest.chunks.size()) return false;
    const Chunk& chunk = manifest.chunks[resource.chunk];
    if (chunk.package >= manifest.packages || chunk.compressed < 4 || chunk.decompressed > nevr_maps::kRecipeResultMax ||
        !Inside(resource.offset, resource.size, chunk.decompressed)) {
      return false;
    }
    File file;
    FileStamp stamp;
    const std::string path = package_prefix + "_" + std::to_string(chunk.package);
    if (!OpenStamped(path, &file, &stamp, read_from) || !Inside(chunk.offset, chunk.compressed, stamp.size)) return false;
    const bool whole = resource.offset == 0 && resource.size == chunk.decompressed;
    std::vector<std::uint8_t> block;
    std::vector<std::uint8_t>* target = whole ? out : &block;
    target->resize(chunk.decompressed);
    if (!DecompressFrame(file, chunk.offset, chunk.compressed, target)) {
      out->clear();
      return false;
    }
    if (!whole) out->assign(block.begin() + resource.offset, block.begin() + resource.offset + resource.size);
    return true;
  }

  static bool ReadWhole(const std::string& path, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) {
    File file;
    FileStamp stamp;
    if (!OpenStamped(path, &file, &stamp, read_from) || stamp.size > nevr_maps::kRecipeResultMax) return false;
    out->resize(static_cast<std::size_t>(stamp.size));
    return out->empty() || file.Read(0, out->data(), out->size());
  }

  bool ReadApkEntry(const std::string& entry_name, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) {
    File file;
    FileStamp stamp;
    if (!OpenStamped(apk_, &file, &stamp, read_from)) return false;
    nevr_quest::data::ZipLimits limits;
    limits.max_entries = 16384;
    limits.max_entry_bytes = nevr_maps::kRecipeResultMax;
    limits.max_total_bytes = ~0ull;
    std::vector<nevr_quest::data::ZipEntry> entries;
    if (nevr_quest::data::ReadZipListing(apk_, limits, &entries, nullptr) != nevr_quest::data::ZipStatus::kOk) return false;
    const nevr_quest::data::ZipEntry* entry = nevr_quest::data::FindZipEntry(entries, entry_name);
    if (entry == nullptr) return false;
    const auto sink = [out](const unsigned char* bytes, std::size_t length) {
      out->insert(out->end(), bytes, bytes + length);
      return out->size() <= nevr_maps::kRecipeResultMax;
    };
    if (nevr_quest::data::ReadZipEntry(apk_, *entry, sink, nullptr) != nevr_quest::data::ZipStatus::kOk) {
      out->clear();
      return false;
    }
    return true;
  }

  DataRootFn data_root_;
  std::string lib_dir_, apk_;
  std::mutex mutex_;
  std::map<std::string, Manifest> manifests_;
};

}  // namespace

std::unique_ptr<nevr_maps::StockSource> MakePosixStockSource(DataRootFn data_root, std::string native_lib_dir,
                                                             std::string apk_path) {
  return std::unique_ptr<nevr_maps::StockSource>(
      new PosixStockSource(data_root, std::move(native_lib_dir), std::move(apk_path)));
}

}  // namespace nevr_quest::maps

#include "quest/cargo/stock_source.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "core/device_memory.h"
#include "core/stock_source.h"
#include "quest/cargo/posix_map_fs.h"
#include "quest/data/plan.h"
#include "quest/data/zip.h"

namespace nevr_quest::maps {
namespace {

using nevr_maps::FileStamp;
using nevr_maps::ManifestPlace;
using nevr_maps::StockFile;
using nevr_maps::StockStamp;

constexpr const char* kApkLibDir = "lib/arm64-v8a/";
constexpr const char* kStockSuffix = ".stock";
// A manifest's file name is its package set's id as 16 lowercase hex digits ("%016llx"; docs/reference/package-manifest-format.md).
constexpr std::size_t kManifestNameChars = 16;
constexpr std::size_t kHex16Bytes = kManifestNameChars + 1;
// A ZIP entry's name length is a u16 (APPNOTE.TXT 4.4.10): what one listed entry can cost beside its record, for the listing's bound.
constexpr std::size_t kZipNameMaxBytes = 0xFFFF;

std::string Hex16(std::uint64_t value) {
  char text[kHex16Bytes];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

bool IsManifestName(const std::string& name) {
  return name.size() == kManifestNameChars &&
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

class PosixStockFile : public StockFile {
 public:
  explicit PosixStockFile(int fd) : fd_(fd) {}
  ~PosixStockFile() override { ::close(fd_); }
  PosixStockFile(const PosixStockFile&) = delete;
  PosixStockFile& operator=(const PosixStockFile&) = delete;

  bool Stamp(FileStamp* out) override {
    PosixMapFs fs;
    return fs.StampOf(fd_, out);
  }

  bool Read(std::uint64_t offset, void* buffer, std::size_t length) override {
    auto* out = static_cast<std::uint8_t*>(buffer);
    while (length > 0) {
      const ssize_t got = ::pread(fd_, out, length, static_cast<off_t>(offset));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) return false;
      out += got;
      offset += static_cast<std::uint64_t>(got);
      length -= static_cast<std::size_t>(got);
    }
    return true;
  }

 private:
  int fd_;
};

class PosixStockSource : public nevr_maps::PackageStockSource {
 public:
  PosixStockSource(DataRootFn data_root, std::string native_lib_dir, std::string apk_path)
      : PackageStockSource(&nevr_device::MemoryAvailable),
        data_root_(data_root),
        lib_dir_(std::move(native_lib_dir)),
        apk_(std::move(apk_path)) {}

  bool ReadScript(std::uint64_t hash, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) override {
    out->clear();
    const std::string file_name = "lib" + Hex16(hash) + ".so";
    if (!lib_dir_.empty()) {
      const std::string path = lib_dir_ + "/" + file_name;
      if (IsFile(path)) return ReadWholeFile(path, out, read_from);
    }
    return !apk_.empty() && ReadApkEntry(std::string(kApkLibDir) + file_name, out, read_from);
  }

 protected:
  std::vector<ManifestPlace> Manifests() override {
    std::vector<ManifestPlace> places;
    const char* root = data_root_ != nullptr ? data_root_() : nullptr;
    if (root == nullptr || *root == '\0') return places;
    const std::string data_dir = std::string(root) + "/_data";
    for (const std::string& folder : ListNames(data_dir)) {
      if (!IsPlainName(folder)) continue;
      const std::string android = data_dir + "/" + folder + "/rad15/android";
      const std::string manifests = android + "/manifests";
      if (!IsDir(manifests)) continue;
      for (const std::string& id : ManifestIds(manifests)) {
        const std::string stock = manifests + "/" + id + kStockSuffix;
        places.push_back({IsFile(stock) ? stock : manifests + "/" + id, android + "/packages/" + id});
      }
    }
    return places;
  }

  std::unique_ptr<StockFile> Open(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      ::close(fd);
      return nullptr;
    }
    return std::unique_ptr<StockFile>(new PosixStockFile(fd));
  }

 private:
  // The pinned manifest first, then every other manifest name in order; `.stock` copies are reached through their id.
  static std::vector<std::string> ManifestIds(const std::string& manifests_dir) {
    const std::string pinned_path = nevr_quest::data::kManifestPath;
    const std::string pinned = pinned_path.substr(pinned_path.rfind('/') + 1);
    std::vector<std::string> ids;
    for (std::string& name : ListNames(manifests_dir)) {
      if (name.size() == kManifestNameChars + std::strlen(kStockSuffix) &&
          name.compare(kManifestNameChars, std::string::npos, kStockSuffix) == 0) {
        name.resize(kManifestNameChars);
      }
      if (IsManifestName(name) && name != pinned) ids.push_back(std::move(name));
    }
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    ids.insert(ids.begin(), pinned);
    return ids;
  }

  // One entry of the APK, bounded as a whole file is (the recipe format's cap and the memory share); the listing's entry count is
  // bounded by the same share, each entry costing its record and the longest name a ZIP can hold.
  bool ReadApkEntry(const std::string& entry_name, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) {
    std::unique_ptr<StockFile> file = Open(apk_);
    FileStamp stamp;
    if (file == nullptr || !file->Stamp(&stamp)) return false;
    read_from->push_back({apk_, stamp});
    const std::uint64_t memory = ObjectBytes();
    const std::uint64_t cap = std::min<std::uint64_t>(nevr_maps::kRecipeResultMax, memory);
    nevr_quest::data::ZipLimits limits;
    limits.max_entries = static_cast<std::size_t>(memory / (sizeof(nevr_quest::data::ZipEntry) + kZipNameMaxBytes));
    limits.max_entry_bytes = cap;
    limits.max_total_bytes = ~std::uint64_t{0};  // the listing allocates no entry's bytes; the sink below bounds the one it reads
    std::vector<nevr_quest::data::ZipEntry> entries;
    if (nevr_quest::data::ReadZipListing(apk_, limits, &entries, nullptr) != nevr_quest::data::ZipStatus::kOk) return false;
    const nevr_quest::data::ZipEntry* entry = nevr_quest::data::FindZipEntry(entries, entry_name);
    if (entry == nullptr) return false;
    const auto sink = [out, cap](const unsigned char* bytes, std::size_t length) {
      if (length > cap - std::min<std::uint64_t>(cap, out->size())) return false;
      out->insert(out->end(), bytes, bytes + length);
      return true;
    };
    if (nevr_quest::data::ReadZipEntry(apk_, *entry, sink, nullptr) != nevr_quest::data::ZipStatus::kOk) {
      out->clear();
      return false;
    }
    return true;
  }

  DataRootFn data_root_;
  std::string lib_dir_, apk_;
};

}  // namespace

std::unique_ptr<nevr_maps::StockSource> MakePosixStockSource(DataRootFn data_root, std::string native_lib_dir,
                                                             std::string apk_path) {
  return std::unique_ptr<nevr_maps::StockSource>(
      new PosixStockSource(data_root, std::move(native_lib_dir), std::move(apk_path)));
}

}  // namespace nevr_quest::maps

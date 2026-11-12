#include "runtime/content/stock_source_win.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include "runtime/server/installed_levels.h"

namespace nevr_stock_source {
namespace {

using nevr_maps::FileStamp;
using nevr_maps::StockStamp;

// docs/reference/package-manifest-format.md: a 24-byte container around one zstd frame; the body's counts and arrays.
constexpr std::size_t kContainerBytes = 24;
constexpr std::uint32_t kContainerVersion = 0x10;
constexpr std::uint64_t kArraysAt = 0xc0, kResourceBytes = 0x20, kFileBytes = 0x28, kChunkBytes = 0x10;
constexpr std::size_t kResourceCountAt = 0x38, kFileCountAt = 0x78, kChunkCountAt = 0xb8;
// A real manifest is 2 MB on disk and 5 MB decompressed; the caps only stop a corrupt length from allocating without bound.
constexpr std::uint64_t kMaxManifestFileBytes = 64ull << 20, kMaxManifestBodyBytes = 256ull << 20;
constexpr std::size_t kReadStep = 1u << 20;

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

class File {
 public:
  File() = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  ~File() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }

  // Full sharing: a reader here never stops the game, the launcher or an update from opening, writing or replacing the file.
  bool Open(const std::filesystem::path& path, DWORD access) {
    handle_ = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle_ != INVALID_HANDLE_VALUE;
  }

  bool Stamp(FileStamp* out) const {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle_, &info)) return false;
    FileStamp stamp;
    stamp.volume = info.dwVolumeSerialNumber;
    stamp.id_lo = (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow;
    stamp.size = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
    stamp.last_write = (std::uint64_t{info.ftLastWriteTime.dwHighDateTime} << 32) | info.ftLastWriteTime.dwLowDateTime;
    FILE_ID_INFO id;
    if (GetFileInformationByHandleEx(handle_, FileIdInfo, &id, sizeof(id))) {
      stamp.volume = id.VolumeSerialNumber;
      std::memcpy(&stamp.id_lo, id.FileId.Identifier, 8);
      std::memcpy(&stamp.id_hi, id.FileId.Identifier + 8, 8);
    }
    *out = stamp;
    return true;
  }

  bool Read(std::uint64_t offset, void* buffer, std::size_t length) const {
    auto* out = static_cast<std::uint8_t*>(buffer);
    while (length > 0) {
      const DWORD want = static_cast<DWORD>(std::min(length, kReadStep));
      OVERLAPPED where = {};
      where.Offset = static_cast<DWORD>(offset);
      where.OffsetHigh = static_cast<DWORD>(offset >> 32);
      DWORD got = 0;
      if (!ReadFile(handle_, out, want, &got, &where) || got == 0) return false;
      out += got;
      offset += got;
      length -= got;
    }
    return true;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// Opens, stamps and records `path` in `read_from`.
bool OpenStamped(const std::filesystem::path& path, File* file, FileStamp* stamp, std::vector<StockStamp>* read_from) {
  if (!file->Open(path, GENERIC_READ) || !file->Stamp(stamp)) return false;
  read_from->push_back({path.u8string(), *stamp});
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

// Decompresses the one zstd frame of `compressed` bytes at `offset` in `file` into exactly `out->size()` bytes, reading
// kReadStep at a time so a large resource costs its own size plus one step.
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

bool ReadWhole(const std::filesystem::path& path, std::uint64_t max_bytes, std::vector<std::uint8_t>* out,
               std::vector<StockStamp>* read_from) {
  File file;
  FileStamp stamp;
  if (!OpenStamped(path, &file, &stamp, read_from) || stamp.size > max_bytes) return false;
  out->resize(static_cast<std::size_t>(stamp.size));
  return out->empty() || file.Read(0, out->data(), out->size());
}

class WinStockSource : public nevr_maps::StockSource {
 public:
  explicit WinStockSource(const std::wstring& install_root) : root_(install_root) {}

  bool ReadResource(std::uint64_t type, std::uint64_t name, std::vector<std::uint8_t>* out,
                    std::vector<StockStamp>* read_from) override {
    std::lock_guard<std::mutex> lock(mutex_);
    out->clear();
    const std::vector<std::filesystem::path> dirs =
        nevr_game_server::FindManifestDirs(root_ / L"bin" / L"win10" / L"echovr.exe");
    for (const std::filesystem::path& dir : dirs) {
      for (const std::uint64_t set : nevr_game_server::BootPackageSets()) {
        const Manifest* manifest = LoadManifest(dir / Hex16(set), read_from);
        if (manifest == nullptr) continue;
        Resource key;
        key.type = type;
        key.name = name;
        const auto it = std::lower_bound(manifest->resources.begin(), manifest->resources.end(), key, ResourceLess);
        if (it == manifest->resources.end() || it->type != type || it->name != name) continue;
        return ReadFromPackage(dir.parent_path() / L"packages", Hex16(set), *manifest, *it, out, read_from);
      }
    }
    return false;
  }

  bool ReadScript(std::uint64_t hash, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) override {
    out->clear();
    const std::filesystem::path path = root_ / L"bin" / L"win10" / L"scripts" / (Hex16(hash) + ".dll");
    return ReadWhole(path, nevr_maps::kRecipeResultMax, out, read_from);
  }

  bool StampsCurrent(const std::vector<StockStamp>& stamps) override {
    for (const StockStamp& recorded : stamps) {
      File file;
      FileStamp now;
      if (!file.Open(std::filesystem::u8path(recorded.path), FILE_READ_ATTRIBUTES) || !file.Stamp(&now)) return false;
      if (now != recorded.stamp) return false;
    }
    return true;
  }

 private:
  // The parsed manifest at `path`, re-read only when its stamp changed; null when it is absent or does not parse. Every
  // manifest consulted is recorded: a key added to an earlier one would change which file serves it.
  const Manifest* LoadManifest(const std::filesystem::path& path, std::vector<StockStamp>* read_from) {
    File file;
    FileStamp stamp;
    if (!OpenStamped(path, &file, &stamp, read_from)) return nullptr;
    const auto found = manifests_.find(path.wstring());
    const bool fresh = found == manifests_.end() || found->second.stamp != stamp;
    Manifest& cached = manifests_[path.wstring()];
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

  bool ReadFromPackage(const std::filesystem::path& packages_dir, const std::string& set_name, const Manifest& manifest,
                       const Resource& resource, std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) {
    if (resource.chunk >= manifest.chunks.size()) return false;
    const Chunk& chunk = manifest.chunks[resource.chunk];
    if (chunk.package >= manifest.packages || chunk.compressed < 4 || chunk.decompressed > nevr_maps::kRecipeResultMax ||
        !Inside(resource.offset, resource.size, chunk.decompressed)) {
      return false;
    }
    File file;
    FileStamp stamp;
    const std::filesystem::path path = packages_dir / (set_name + "_" + std::to_string(chunk.package));
    if (!OpenStamped(path, &file, &stamp, read_from) || !Inside(chunk.offset, chunk.compressed, stamp.size)) return false;
    // A large resource is its chunk's whole content, so it decompresses straight into `out`.
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

  std::filesystem::path root_;
  std::mutex mutex_;
  std::map<std::wstring, Manifest> manifests_;
};

}  // namespace

std::unique_ptr<nevr_maps::StockSource> MakeWinStockSource(const std::wstring& install_root) {
  return std::unique_ptr<nevr_maps::StockSource>(new WinStockSource(install_root));
}

}  // namespace nevr_stock_source

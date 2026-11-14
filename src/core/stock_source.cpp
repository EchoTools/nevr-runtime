#include "core/stock_source.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "core/cargo_limits.h"
#include "core/device_memory.h"
#include "core/map_format.h"

namespace nevr_maps {

namespace {

// docs/reference/package-manifest-format.md: a 24-byte container (ASCII "ZSTD", u32 version 0x10, u64 decompressed length, u64
// compressed length) around one zstd frame; then the body's three counts and its arrays, back to back from 0xc0.
constexpr char kContainerMagic[] = "ZSTD";
constexpr std::size_t kContainerMagicBytes = sizeof(kContainerMagic) - 1;
constexpr std::size_t kContainerVersionAt = 4, kContainerBodyBytesAt = 8, kContainerCompressedAt = 16;
constexpr std::size_t kContainerBytes = 24;
constexpr std::uint32_t kContainerVersion = 0x10;
constexpr std::size_t kPackageCountAt = 0x00;
constexpr std::size_t kResourceCountAt = 0x38, kFileCountAt = 0x78, kChunkCountAt = 0xb8;
constexpr std::uint64_t kArraysAt = 0xc0;
constexpr std::uint64_t kResourceBytes = 0x20, kFileBytes = 0x28, kChunkBytes = 0x10;
// A resource record: u64 type, u64 name, u32 chunk, u32 offset in the chunk, u32 size, u32 (unused here).
constexpr std::size_t kResourceNameAt = 0x08, kResourceChunkAt = 0x10, kResourceOffsetAt = 0x14, kResourceSizeAt = 0x18;
// A chunk record: u32 package, u32 offset, u32 compressed size, u32 decompressed size.
constexpr std::size_t kChunkOffsetAt = 4, kChunkCompressedAt = 8, kChunkDecompressedAt = 12;
// The smallest zstd frame is its 4-byte magic and more (RFC 8878 section 3.1.1): a chunk of fewer bytes is no frame.
constexpr std::uint32_t kZstdMagicBytes = 4;
constexpr unsigned kU32Width = 4, kU64Width = 8, kBitsPerByte = 8;
// One read of a package while its chunk decompresses: at most this many compressed bytes are held at once beside the output.
constexpr std::size_t kReadStepBytes = std::size_t{1} << 20;

bool Inside(std::uint64_t offset, std::uint64_t length, std::uint64_t total) {
  return offset <= total && length <= total - offset;
}

std::uint64_t Load(const std::uint8_t* p, unsigned width) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) value |= std::uint64_t{p[i]} << (kBitsPerByte * i);
  return value;
}

bool ResourceLess(std::uint64_t a_type, std::uint64_t a_name, std::uint64_t b_type, std::uint64_t b_name) {
  return a_type != b_type ? a_type < b_type : a_name < b_name;
}

// Decompresses the one zstd frame of `compressed` bytes at `offset` in `file` into exactly `out->size()` bytes, reading
// kReadStepBytes at a time, so a large resource costs its own size plus one step.
bool DecompressFrame(StockFile& file, std::uint64_t offset, std::uint64_t compressed, std::vector<std::uint8_t>* out) {
  std::unique_ptr<ZSTD_DCtx, std::size_t (*)(ZSTD_DCtx*)> context(ZSTD_createDCtx(), ZSTD_freeDCtx);
  if (!context) return false;
  std::vector<std::uint8_t> step(static_cast<std::size_t>(std::min<std::uint64_t>(compressed, kReadStepBytes)));
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

}  // namespace

PackageStockSource::PackageStockSource(MemoryFn memory_available) : memory_available_(memory_available) {}

// The one-object share of the device's memory (cargo_limits.h kMemoryShareForObject), the same share the rebuild holds a recipe's
// bytes to (map_derive.h DeriveMemoryOf, object_bytes).
std::uint64_t PackageStockSource::ObjectBytes() const {
  std::uint64_t memory = 0;
  if (memory_available_ == nullptr || !memory_available_(&memory)) return 0;
  return nevr_device::Share(memory, nevr_cargo::kMemoryShareForObject);
}

std::unique_ptr<StockFile> PackageStockSource::OpenStamped(const std::string& path, FileStamp* stamp,
                                                           std::vector<StockStamp>* read_from) {
  std::unique_ptr<StockFile> file = Open(path);
  if (file == nullptr || !file->Stamp(stamp)) return nullptr;
  read_from->push_back({path, *stamp});
  return file;
}

bool PackageStockSource::ReadResource(std::uint64_t type, std::uint64_t name, std::vector<std::uint8_t>* out,
                                      std::vector<StockStamp>* read_from) {
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  for (const ManifestPlace& place : Manifests()) {
    const Manifest* manifest = LoadManifest(place.manifest_path, read_from);
    if (manifest == nullptr) continue;
    Resource key;
    key.type = type;
    key.name = name;
    const auto it = std::lower_bound(manifest->resources.begin(), manifest->resources.end(), key,
                                     [](const Resource& a, const Resource& b) { return ResourceLess(a.type, a.name, b.type, b.name); });
    if (it == manifest->resources.end() || it->type != type || it->name != name) continue;
    return ReadFromPackage(place.package_prefix, *manifest, *it, out, read_from);
  }
  return false;
}

bool PackageStockSource::StampsCurrent(const std::vector<StockStamp>& stamps) {
  for (const StockStamp& recorded : stamps) {
    std::unique_ptr<StockFile> file = Open(recorded.path);
    FileStamp now;
    if (file == nullptr || !file->Stamp(&now) || now != recorded.stamp) return false;
  }
  return true;
}

bool PackageStockSource::ReadWholeFile(const std::string& path, std::vector<std::uint8_t>* out,
                                       std::vector<StockStamp>* read_from) {
  out->clear();
  FileStamp stamp;
  std::unique_ptr<StockFile> file = OpenStamped(path, &stamp, read_from);
  if (file == nullptr || stamp.size > kRecipeResultMax || stamp.size > ObjectBytes()) return false;
  out->resize(static_cast<std::size_t>(stamp.size));
  if (!out->empty() && !file->Read(0, out->data(), out->size())) {
    out->clear();
    return false;
  }
  return true;
}

// The parsed manifest at `path`, read again only when its stamp changed; null when it is absent, does not parse, or is over the
// memory share. Every manifest consulted is recorded: a key added to an earlier one would change which file serves it.
const PackageStockSource::Manifest* PackageStockSource::LoadManifest(const std::string& path,
                                                                     std::vector<StockStamp>* read_from) {
  FileStamp stamp;
  std::unique_ptr<StockFile> file = OpenStamped(path, &stamp, read_from);
  if (file == nullptr) return nullptr;
  const auto found = manifests_.find(path);
  if (found != manifests_.end() && found->second.stamp == stamp) return found->second.ok ? &found->second : nullptr;
  Manifest& cached = manifests_[path];
  cached = Manifest{};
  cached.stamp = stamp;
  const std::uint64_t memory = ObjectBytes();
  if (stamp.size >= kContainerBytes && stamp.size <= memory) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stamp.size));
    if (file->Read(0, bytes.data(), bytes.size())) ParseManifest(bytes, memory - stamp.size, &cached);
  }
  return cached.ok ? &cached : nullptr;
}

// `memory` is what the decompressed body may take beside the file already held.
bool PackageStockSource::ParseManifest(const std::vector<std::uint8_t>& file, std::uint64_t memory, Manifest* out) {
  if (file.size() < kContainerBytes || std::memcmp(file.data(), kContainerMagic, kContainerMagicBytes) != 0) return false;
  if (Load(file.data() + kContainerVersionAt, kU32Width) != kContainerVersion) return false;
  const std::uint64_t body_bytes = Load(file.data() + kContainerBodyBytesAt, kU64Width);
  const std::uint64_t compressed = Load(file.data() + kContainerCompressedAt, kU64Width);
  if (compressed > file.size() - kContainerBytes) return false;
  if (body_bytes < kArraysAt || body_bytes > memory) return false;
  const unsigned long long declared =
      ZSTD_getFrameContentSize(file.data() + kContainerBytes, static_cast<std::size_t>(compressed));
  if (declared == ZSTD_CONTENTSIZE_ERROR || (declared != ZSTD_CONTENTSIZE_UNKNOWN && declared != body_bytes)) return false;
  std::vector<std::uint8_t> body(static_cast<std::size_t>(body_bytes));
  const std::size_t written =
      ZSTD_decompress(body.data(), body.size(), file.data() + kContainerBytes, static_cast<std::size_t>(compressed));
  if (ZSTD_isError(written) || written != body.size()) return false;

  const std::uint64_t resources = Load(body.data() + kResourceCountAt, kU64Width);
  const std::uint64_t files = Load(body.data() + kFileCountAt, kU64Width);
  const std::uint64_t chunks = Load(body.data() + kChunkCountAt, kU64Width);
  std::uint64_t room = body_bytes - kArraysAt;
  if (resources > room / kResourceBytes) return false;
  room -= resources * kResourceBytes;
  if (files > room / kFileBytes) return false;
  room -= files * kFileBytes;
  if (chunks > room / kChunkBytes) return false;

  out->packages = static_cast<std::uint32_t>(Load(body.data() + kPackageCountAt, kU32Width));
  out->resources.resize(static_cast<std::size_t>(resources));
  for (std::size_t i = 0; i < out->resources.size(); ++i) {
    const std::uint8_t* at = body.data() + kArraysAt + i * kResourceBytes;
    Resource& r = out->resources[i];
    r.type = Load(at, kU64Width);
    r.name = Load(at + kResourceNameAt, kU64Width);
    r.chunk = static_cast<std::uint32_t>(Load(at + kResourceChunkAt, kU32Width));
    r.offset = static_cast<std::uint32_t>(Load(at + kResourceOffsetAt, kU32Width));
    r.size = static_cast<std::uint32_t>(Load(at + kResourceSizeAt, kU32Width));
  }
  std::sort(out->resources.begin(), out->resources.end(),
            [](const Resource& a, const Resource& b) { return ResourceLess(a.type, a.name, b.type, b.name); });
  const std::uint8_t* chunk_table = body.data() + kArraysAt + resources * kResourceBytes + files * kFileBytes;
  out->chunks.resize(static_cast<std::size_t>(chunks));
  for (std::size_t i = 0; i < out->chunks.size(); ++i) {
    const std::uint8_t* at = chunk_table + i * kChunkBytes;
    Chunk& c = out->chunks[i];
    c.package = static_cast<std::uint32_t>(Load(at, kU32Width));
    c.offset = static_cast<std::uint32_t>(Load(at + kChunkOffsetAt, kU32Width));
    c.compressed = static_cast<std::uint32_t>(Load(at + kChunkCompressedAt, kU32Width));
    c.decompressed = static_cast<std::uint32_t>(Load(at + kChunkDecompressedAt, kU32Width));
  }
  out->ok = true;
  return true;
}

bool PackageStockSource::ReadFromPackage(const std::string& package_prefix, const Manifest& manifest, const Resource& resource,
                                         std::vector<std::uint8_t>* out, std::vector<StockStamp>* read_from) {
  if (resource.chunk >= manifest.chunks.size()) return false;
  const Chunk& chunk = manifest.chunks[resource.chunk];
  if (chunk.package >= manifest.packages || chunk.compressed < kZstdMagicBytes || chunk.decompressed > kRecipeResultMax ||
      chunk.decompressed > ObjectBytes() || !Inside(resource.offset, resource.size, chunk.decompressed)) {
    return false;
  }
  FileStamp stamp;
  std::unique_ptr<StockFile> file = OpenStamped(package_prefix + "_" + std::to_string(chunk.package), &stamp, read_from);
  if (file == nullptr || !Inside(chunk.offset, chunk.compressed, stamp.size)) return false;
  // A large resource is its chunk's whole content, so it decompresses straight into `out`.
  const bool whole = resource.offset == 0 && resource.size == chunk.decompressed;
  std::vector<std::uint8_t> block;
  std::vector<std::uint8_t>* target = whole ? out : &block;
  target->resize(chunk.decompressed);
  if (!DecompressFrame(*file, chunk.offset, chunk.compressed, target)) {
    out->clear();
    return false;
  }
  if (!whole) out->assign(block.begin() + resource.offset, block.begin() + resource.offset + resource.size);
  return true;
}

}  // namespace nevr_maps

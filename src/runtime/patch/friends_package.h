#pragma once

// #1535 / #1599: the FRIENDS panel's own package set, written by the runtime: the avatar textures the panel's and the
// profile's pictures show (a flat placeholder until a friend's picture replaces it). Everything in it is made here
// (pixels, DDS header, texture descriptor, manifest), nothing from the game's data, so nothing derived from it is
// stored or shipped; the runtime writes the set under the nEVR root and adds it with the engine's own AddPackage.
//
// A texture is two resources of one name: its descriptor (on-disk type Make(CGTextureResource, "Win10"), 256 bytes)
// and its payload (Make(CGTextureResource, "Win10GPU"), a DDS file). The descriptor, as every stock uncompressed
// texture has it: 192 bytes of 0xff, then u32s: 1, width, height, 1, mip count, 0, DXGI format, 0, 0, 0, width,
// height, 1, the payload's byte size, the row pitch, 0.
//
// A package set: rad15/<platform>/manifests/<set id> and rad15/<platform>/packages/<set id>_0. The manifest file is
// "ZSTD", u32 16, u64 raw size, u64 compressed size, then one zstd frame of: a 0xC0-byte header (u32 package count, u32
// flags = the block size 0x80000; three sections at 0x10, 0x50, 0x90, each: u64 byte length, u64 0x100000000 at +0x10,
// u64 32, count, count at +0x18), the resource records (32 bytes: type, name, u32 block, u32 offset in the block, u32
// size, u16 0x10, u16 0), one info record per resource (40 bytes: type, name, 24 zero bytes), the block records
// (16 bytes: u32 package, u32 offset, u32 compressed size, u32 size), an end record per package (package, its size,
// 0, 0) and a zero record. Resources and infos are in signed (type, name) order (the engine searches them). The
// package is the blocks' zstd frames, one after another, each at most 512 KiB uncompressed. This is the format
// tools/echomod/rad_archive_tool.py repacks the game's own sets in.
//
// Pure logic, header only, no platform headers (zstd only), no exceptions. Tests: src/runtime/tests/test_friends_package.cpp.

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/patch/friends_canvas.h"

namespace nevr_friends_package {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint64_t kTextureDescriptorType = 0x4A4C32C49300B8A0ULL;  // Make(CGTextureResource, "Win10")
inline constexpr std::uint64_t kTexturePayloadType = 0xBEAC1969CB7B8861ULL;     // Make(CGTextureResource, "Win10GPU")

// The two platforms' textures. PC: a 256-byte descriptor and a DDS payload of B8G8R8A8 pixels. Quest: a 248-byte
// descriptor (the same fields without the row pitch) and a payload of raw ASTC 8x8 blocks, no header (every texture of
// the Quest's data is compressed; its descriptors keep PC's format number, 87, for those). The set's manifest and package
// are the same on both, under rad15/win10 or rad15/android.
struct Platform {
  std::uint64_t descriptor_type;
  std::uint64_t payload_type;
  bool astc;
  const char* directory;
};
inline constexpr Platform kWin10{kTextureDescriptorType, kTexturePayloadType, false, "win10"};
inline constexpr Platform kAndroid{0xE2EFE7289D5985B8ULL, 0x489BB35D53CA50E9ULL, true, "android"};  // Make(..., "Android" / "AndroidGPU")
inline constexpr std::uint32_t kAstcBlock = 8;  // the Quest's block size (8 x 8: 16 bytes per 64 pixels)
inline constexpr std::uint32_t kDxgiB8G8R8A8 = 87;
inline constexpr std::uint32_t kBlockSize = 0x80000;
inline constexpr std::uint32_t kPictureSide = 128;
inline constexpr std::array<std::uint8_t, 4> kPlaceholderBgra = {0x49, 0x3D, 0x33, 0xFF};  // the avatar slate

inline constexpr std::uint64_t kSetId = EchoVR::CSymbol64Hash("nevr_friends_panel");
static_assert(kSetId == 0x27974AE8287406F2ULL, "the set id the panel adds");

namespace detail {
template <typename T>
void Put(Bytes* b, std::size_t at, T v) {
  std::memcpy(b->data() + at, &v, sizeof(v));
}
template <typename T>
T Get(const Bytes& b, std::size_t at) {
  T v{};
  std::memcpy(&v, b.data() + at, sizeof(v));
  return v;
}
}  // namespace detail

// A DDS file of one 2D B8G8R8A8 image (DX10 header): `bgra` is width x height x 4 bytes.
inline Bytes Dds(std::uint32_t width, std::uint32_t height, const Bytes& bgra) {
  using detail::Put;
  Bytes b(148, 0);
  std::memcpy(b.data(), "DDS ", 4);
  Put<std::uint32_t>(&b, 4, 124);                // header size
  Put<std::uint32_t>(&b, 8, 0x0002100F);         // caps, height, width, pitch, pixel format, mip count
  Put<std::uint32_t>(&b, 12, height);
  Put<std::uint32_t>(&b, 16, width);
  Put<std::uint32_t>(&b, 20, width * 4);         // row pitch
  Put<std::uint32_t>(&b, 24, 1);                 // depth (1, as the stock textures write it)
  Put<std::uint32_t>(&b, 28, 1);                 // mip count
  Put<std::uint32_t>(&b, 76, 32);                // pixel format size
  Put<std::uint32_t>(&b, 80, 4);                 // FOURCC
  std::memcpy(b.data() + 84, "DX10", 4);
  Put<std::uint32_t>(&b, 108, 0x1000);           // texture
  Put<std::uint32_t>(&b, 128, kDxgiB8G8R8A8);
  Put<std::uint32_t>(&b, 132, 3);                // 2D
  Put<std::uint32_t>(&b, 140, 1);                // array size
  b.insert(b.end(), bgra.begin(), bgra.end());
  return b;
}

// The descriptor of a one-mip B8G8R8A8 texture whose payload (`payload_bytes`, header included) is a DDS file.
inline Bytes TextureDescriptor(std::uint32_t width, std::uint32_t height, std::uint32_t payload_bytes) {
  Bytes b(256, 0xFF);
  const std::array<std::uint32_t, 16> fields = {1, width, height, 1, 1, 0, kDxgiB8G8R8A8, 0,
                                                0, 0, width, height, 1, payload_bytes, width * 4, 0};
  std::memcpy(b.data() + 192, fields.data(), sizeof(fields));
  return b;
}

struct Resource {
  std::uint64_t type;
  std::uint64_t name;
  Bytes bytes;
};

// The Quest's descriptor of a one-mip texture: as TextureDescriptor without the row pitch, 248 bytes.
inline Bytes AndroidTextureDescriptor(std::uint32_t width, std::uint32_t height, std::uint32_t payload_bytes) {
  Bytes b(248, 0xFF);
  const std::array<std::uint32_t, 14> fields = {1, width, height, 1, 1, 0, kDxgiB8G8R8A8, 0, 0, 0, width, height, 1, payload_bytes};
  std::memcpy(b.data() + 192, fields.data(), sizeof(fields));
  return b;
}

// One ASTC block that paints a single colour (a void-extent block, LDR, no extent): the low word 0xFFFFFFFFFFFFFDFC,
// then red, green, blue, alpha as 16-bit unsigned normalized values.
inline std::array<std::uint8_t, 16> AstcSolidBlock(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
  std::array<std::uint8_t, 16> block{};
  const std::uint64_t marker = 0xFFFFFFFFFFFFFDFCULL;
  std::memcpy(block.data(), &marker, 8);
  const std::array<std::uint16_t, 4> rgba = {static_cast<std::uint16_t>(r * 257), static_cast<std::uint16_t>(g * 257),
                                             static_cast<std::uint16_t>(b * 257), static_cast<std::uint16_t>(a * 257)};
  std::memcpy(block.data() + 8, rgba.data(), 8);
  return block;
}

// The two resources of a placeholder picture named `name`.
inline void AddPlaceholderPicture(const Platform& platform, std::uint64_t name, std::vector<Resource>* out) {
  Bytes payload, descriptor;
  if (platform.astc) {
    const std::size_t blocks = (kPictureSide / kAstcBlock) * (kPictureSide / kAstcBlock);
    const auto block = AstcSolidBlock(kPlaceholderBgra[2], kPlaceholderBgra[1], kPlaceholderBgra[0], kPlaceholderBgra[3]);
    for (std::size_t i = 0; i < blocks; ++i) payload.insert(payload.end(), block.begin(), block.end());
    descriptor = AndroidTextureDescriptor(kPictureSide, kPictureSide, static_cast<std::uint32_t>(payload.size()));
  } else {
    Bytes pixels(static_cast<std::size_t>(kPictureSide) * kPictureSide * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) std::memcpy(pixels.data() + i, kPlaceholderBgra.data(), 4);
    payload = Dds(kPictureSide, kPictureSide, pixels);
    descriptor = TextureDescriptor(kPictureSide, kPictureSide, static_cast<std::uint32_t>(payload.size()));
  }
  out->push_back({platform.descriptor_type, name, std::move(descriptor)});
  out->push_back({platform.payload_type, name, std::move(payload)});
}

struct PackageSet {
  Bytes manifest;  // the manifest file (the ZSTD-wrapped frame)
  Bytes package;   // package 0
};

namespace detail {

inline bool Compress(const Bytes& raw, Bytes* out) {
  ZSTD_CCtx* ctx = ZSTD_createCCtx();
  if (ctx == nullptr) return false;
  ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, 3);
  ZSTD_CCtx_setParameter(ctx, ZSTD_c_windowLog, 19);  // 512 KiB, the engine's window
  ZSTD_CCtx_setParameter(ctx, ZSTD_c_contentSizeFlag, 1);
  out->resize(ZSTD_compressBound(raw.size()));
  const std::size_t n = ZSTD_compress2(ctx, out->data(), out->size(), raw.data(), raw.size());
  ZSTD_freeCCtx(ctx);
  if (ZSTD_isError(n) != 0) return false;
  out->resize(n);
  return true;
}

inline bool SignedLess(const Resource& a, const Resource& b) {
  const auto at = static_cast<std::int64_t>(a.type), bt = static_cast<std::int64_t>(b.type);
  if (at != bt) return at < bt;
  return static_cast<std::int64_t>(a.name) < static_cast<std::int64_t>(b.name);
}

}  // namespace detail

// Packs `resources` (one per (type, name), none empty) as one package. False when one is empty, a pair repeats, or
// compression fails.
inline bool BuildPackageSet(std::vector<Resource> resources, PackageSet* out) {
  using detail::Put;
  std::sort(resources.begin(), resources.end(), detail::SignedLess);
  for (std::size_t i = 0; i < resources.size(); ++i) {
    if (resources[i].bytes.empty()) return false;
    if (i > 0 && resources[i].type == resources[i - 1].type && resources[i].name == resources[i - 1].name) return false;
  }
  // Blocks of at most kBlockSize bytes, resources in the sorted order (a resource larger than a block gets its own).
  struct Placed {
    std::uint32_t block, offset;
  };
  std::vector<Placed> placed(resources.size());
  std::vector<Bytes> blocks;
  for (std::size_t i = 0; i < resources.size(); ++i) {
    if (blocks.empty() || (!blocks.back().empty() && blocks.back().size() + resources[i].bytes.size() > kBlockSize))
      blocks.emplace_back();
    placed[i] = {static_cast<std::uint32_t>(blocks.size() - 1), static_cast<std::uint32_t>(blocks.back().size())};
    blocks.back().insert(blocks.back().end(), resources[i].bytes.begin(), resources[i].bytes.end());
  }
  Bytes package;
  struct Block {
    std::uint32_t offset, compressed, size;
  };
  std::vector<Block> block_records;
  for (const Bytes& raw : blocks) {
    Bytes frame;
    if (!detail::Compress(raw, &frame)) return false;
    block_records.push_back({static_cast<std::uint32_t>(package.size()), static_cast<std::uint32_t>(frame.size()),
                             static_cast<std::uint32_t>(raw.size())});
    package.insert(package.end(), frame.begin(), frame.end());
  }
  const std::size_t n = resources.size(), nb = block_records.size() + 2;  // + the package end and the zero record
  Bytes m(0xC0 + 32 * n + 40 * n + 16 * nb, 0);
  Put<std::uint32_t>(&m, 0, 1);  // one package
  Put<std::uint32_t>(&m, 4, kBlockSize);
  const std::array<std::pair<std::size_t, std::pair<std::size_t, std::size_t>>, 3> sections = {
      {{0x10, {32, n}}, {0x50, {40, n}}, {0x90, {16, nb}}}};
  for (const auto& s : sections) {
    Put<std::uint64_t>(&m, s.first, s.second.first * s.second.second);
    Put<std::uint64_t>(&m, s.first + 0x10, 0x100000000ULL);
    Put<std::uint64_t>(&m, s.first + 0x18, 32);
    Put<std::uint64_t>(&m, s.first + 0x20, s.second.second);
    Put<std::uint64_t>(&m, s.first + 0x28, s.second.second);
  }
  std::size_t at = 0xC0;
  for (std::size_t i = 0; i < n; ++i, at += 32) {
    Put<std::uint64_t>(&m, at, resources[i].type);
    Put<std::uint64_t>(&m, at + 8, resources[i].name);
    Put<std::uint32_t>(&m, at + 16, placed[i].block);
    Put<std::uint32_t>(&m, at + 20, placed[i].offset);
    Put<std::uint32_t>(&m, at + 24, static_cast<std::uint32_t>(resources[i].bytes.size()));
    Put<std::uint16_t>(&m, at + 28, 0x10);
  }
  for (std::size_t i = 0; i < n; ++i, at += 40) {
    Put<std::uint64_t>(&m, at, resources[i].type);
    Put<std::uint64_t>(&m, at + 8, resources[i].name);
  }
  for (const Block& b : block_records) {
    Put<std::uint32_t>(&m, at + 4, b.offset);
    Put<std::uint32_t>(&m, at + 8, b.compressed);
    Put<std::uint32_t>(&m, at + 12, b.size);
    at += 16;
  }
  Put<std::uint32_t>(&m, at + 4, static_cast<std::uint32_t>(package.size()));  // package 0's end
  Bytes frame;
  if (!detail::Compress(m, &frame)) return false;
  Bytes file(24, 0);
  std::memcpy(file.data(), "ZSTD", 4);
  Put<std::uint32_t>(&file, 4, 16);
  Put<std::uint64_t>(&file, 8, m.size());
  Put<std::uint64_t>(&file, 16, frame.size());
  file.insert(file.end(), frame.begin(), frame.end());
  out->manifest = std::move(file);
  out->package = std::move(package);
  return true;
}

// The textures the panel's and the profile's pictures use, named as friends_canvas.h names them.
using nevr_friends_canvas::ProfilePicture;
using nevr_friends_canvas::RowPicture;

// The panel's set: a placeholder picture for every name.
inline bool BuildFriendsPictures(const Platform& platform, std::uint32_t rows, std::uint32_t slots, PackageSet* out) {
  std::vector<Resource> resources;
  for (std::uint32_t r = 0; r < rows; ++r)
    for (std::uint32_t s = 0; s < slots; ++s) AddPlaceholderPicture(platform, RowPicture(r, s), &resources);
  for (std::uint32_t s = 0; s < slots; ++s) AddPlaceholderPicture(platform, ProfilePicture(s), &resources);
  return BuildPackageSet(std::move(resources), out);
}

}  // namespace nevr_friends_package

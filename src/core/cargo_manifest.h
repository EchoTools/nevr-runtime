/* The cargo manifest: the JSON document on the CDN that lists the texture buffers a client may fetch
 * (docs/adr/0044-cdn-texture-delivery.md). One document, `v1/manifest.json` (or the test manifest a config names), keyed by
 * label; every entry is one buffer of one resource, named the way the game names it on disk.
 *
 *   {"schema": "nevr-cargo-manifest", "schema_version": 1,
 *    "labels": {"lobby_posters": {"entries": [
 *       {"platform": "android-arm64", "type": "0xe2efe7289d5985b8", "name": "0x3fa6d25bc24a7b01", "format": "zstd-frame",
 *        "sha256": "<64 hex of the stored frame>", "size": <frame bytes>, "bytes": <decompressed bytes>, "align": <alignment>,
 *        "stock_size": <the stock buffer the art was made against>}, ...]}}}
 *
 * `type` is the on-disk type (the buffer and the platform variant), `name` the resource's symbol, both as "0x" hex strings (a
 * JSON number cannot hold 64 bits). `size` is the stored zstd frame, `bytes` what the engine decompresses it to, `align` the record's
 * alignment. The runtime never decompresses: the SHA-256 proves the frame is the published one and the engine decompresses it.
 *
 * The parser reads with nlohmann::json's non-throwing calls only (the sentinel is built without exceptions), refuses anything that
 * is not exactly this shape, and reports the label and the entry's index, never text from the file (logging Rule 4). A key it does
 * not know is ignored and counted. In particular a manifest cannot set a label's state: states are code defaults and config.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_cargo {

inline constexpr std::size_t kMaxManifestBytes = std::size_t{1} << 20;           // ADR 0024 section 3's bound
inline constexpr std::uint64_t kMaxBlobBytes = std::uint64_t{128} << 20;         // asset_cache.h kMaxBlobBytes
inline constexpr std::uint64_t kDefaultCapBytes = std::uint64_t{512} << 20;      // asset_cache.h kDefaultCapBytes
inline constexpr std::uint32_t kMaxAlignment = 1u << 16;
inline constexpr std::size_t kMaxLabels = 64;
inline constexpr std::size_t kMaxEntries = 2048;
inline constexpr const char* kSchema = "nevr-cargo-manifest";
inline constexpr int kSchemaVersion = 1;
inline constexpr const char* kFormatZstdFrame = "zstd-frame";

// A platform's names for a texture's two buffers (docs/adr/0044 "The per-binary table"; resource_buffer_key.h on PC, the
// Quest log of the poster's two reads for Android).
struct Platform {
  const char* name;
  std::uint64_t buffer0_type;  // the descriptor
  std::uint64_t buffer1_type;  // the pixels
};
inline constexpr Platform kAndroidArm64 = {"android-arm64", 0xe2efe7289d5985b8ULL, 0x489bb35d53ca50e9ULL};
inline constexpr Platform kWindowsX64 = {"windows-x86_64", 0x4a4c32c49300b8a0ULL, 0xbeac1969cb7b8861ULL};

struct Entry {
  std::string label;
  std::string platform;
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  std::string sha256;  // 64 lowercase hex characters
  std::uint64_t size = 0;
  std::uint64_t bytes = 0;
  std::uint32_t align = 0;
  std::uint64_t stock_size = 0;
};

struct Manifest {
  std::vector<std::string> labels;  // in document order
  std::vector<Entry> entries;       // in document order, every platform
  std::uint32_t unknown_fields = 0;  // keys the parser ignored (never named: remote text)
};

enum class ParseStatus {
  kOk = 0,
  kTooLarge,      // over kMaxManifestBytes
  kNotJson,       // not a JSON object
  kSchema,        // wrong or missing "schema"
  kVersion,       // wrong or missing "schema_version"
  kMissingField,  // a required key is absent
  kBadField,      // a key has the wrong JSON type, an out-of-range value or a malformed name
  kBadHash,       // a sha256 that is not 64 lowercase hex characters
  kTooBig,        // size or bytes over kMaxBlobBytes (or zero)
  kBadFormat,     // a format other than "zstd-frame"
  kDuplicate,     // two entries with one (platform, type, name)
  kTooMany,       // more than kMaxLabels labels or kMaxEntries entries
};

const char* ParseStatusName(ParseStatus status) noexcept;

struct ParseResult {
  ParseStatus status = ParseStatus::kOk;
  std::string label;       // the label of the entry that failed (a label name is checked against [a-z0-9_] first)
  std::size_t index = 0;   // the entry's index in that label
};

ParseResult ParseManifest(std::string_view text, Manifest* out);

// "0x" followed by 1 to 16 hex digits.
bool ParseHex64(std::string_view text, std::uint64_t* out) noexcept;
bool IsSha256Hex(std::string_view text) noexcept;

}  // namespace nevr_cargo

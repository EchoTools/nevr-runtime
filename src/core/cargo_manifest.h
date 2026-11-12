/* The cargo manifest: the JSON document, published as one zstd frame, that is on the CDN that lists the game's objects a client may fetch
 * (docs/adr/0045-cdn-texture-delivery.md). One document, `v1/manifest.json` (or the manifest a config names), keyed by label; every
 * entry is one on-disk record of one resource, of any on-disk type, named the way the game names it.
 *
 *   {"schema": "nevr-cargo-manifest", "schema_version": 1,
 *    "labels": {"game": {"entries": [
 *       {"platform": "android-arm64", "type": "0xe2efe7289d5985b8", "name": "0x3fa6d25bc24a7b01", "format": "zstd-frame",
 *        "sha256": "<64 hex of the stored frame>", "size": <frame bytes>, "bytes": <decompressed bytes>, "align": <alignment>,
 *        "stock_size": <optional: the stock buffer the art was made against>}, ...]}},
 *    "empty": [{"type": "0x...", "name": "0x...", "align": <alignment>}, ...]}
 *
 * `type` is the on-disk type, `name` the resource's symbol, both as "0x" hex strings (a JSON number cannot hold 64 bits). `size` is the
 * stored zstd frame, `bytes` what the engine decompresses it to, `align` the record's alignment. The runtime never decompresses: the
 * SHA-256 proves the frame is the published one and the engine decompresses it. `empty` lists the records of 0 bytes, which have no
 * frame; they belong to the platform the plan is made for (one manifest is one platform's set).
 *
 * The parser reads with nlohmann::json's non-throwing calls only (the sentinel is built without exceptions). A document that is not
 * exactly this shape is refused whole. An entry the validator refuses (cargo_validate.h) is left out and listed, by rule, in
 * Manifest::refused: one bad record does not take the other 70,000 with it. No size here is a constant: the text, the entry count and
 * every object's size are held to the Limits computed from the device.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/cargo_format.h"
#include "core/cargo_limits.h"
#include "core/cargo_validate.h"

namespace nevr_cargo {

inline constexpr const char* kSchema = "nevr-cargo-manifest";
inline constexpr int kSchemaVersion = 1;
inline constexpr const char* kFormatZstdFrame = "zstd-frame";

// Two on-disk types that belong together under one name: a descriptor and the payload it describes (a texture's two buffers). The
// per-binary table of them is Platform::pairs. A payload whose bytes differ from the stock buffer's needs its descriptor.
struct PairRule {
  std::uint64_t descriptor;
  std::uint64_t payload;
};

struct Platform {
  const char* name;
  const PairRule* pairs;
  std::size_t pair_count;
};

inline constexpr PairRule kAndroidPairs[] = {{0xe2efe7289d5985b8ULL, 0x489bb35d53ca50e9ULL}};
inline constexpr PairRule kWindowsPairs[] = {{0x4a4c32c49300b8a0ULL, 0xbeac1969cb7b8861ULL}};
inline constexpr Platform kAndroidArm64 = {"android-arm64", kAndroidPairs, 1};
inline constexpr Platform kWindowsX64 = {"windows-x86_64", kWindowsPairs, 1};

struct Entry {
  std::string label;
  std::string platform;
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  std::string sha256;  // 64 lowercase hex characters
  std::uint64_t size = 0;
  std::uint64_t bytes = 0;
  std::uint32_t align = 0;
  bool has_stock_size = false;
  std::uint64_t stock_size = 0;  // meaningful only when has_stock_size
};

struct EmptyRecord {
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  std::uint32_t align = 0;
};

// An entry the validator left out.
struct Refusal {
  std::string label;
  std::size_t index = 0;  // the entry's index in its label (an empty record: in the `empty` list)
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  Rule rule = Rule::kNone;
};

struct Manifest {
  std::vector<std::string> labels;  // in document order
  std::vector<Entry> entries;       // in document order, every platform, validator-accepted
  std::vector<EmptyRecord> empties;
  std::vector<Refusal> refused;
  std::uint32_t unknown_fields = 0;  // keys the parser ignored (never named: remote text)
};

// What one entry costs the run in memory: the entry, its shadow record and its chunk entry. For ComputeLimits.
std::uint64_t ManifestEntryMemoryBytes() noexcept;

enum class ParseStatus {
  kOk = 0,
  kTooLarge,      // more text than Limits::manifest_text_bytes
  kNotJson,       // not a JSON object
  kSchema,        // wrong or missing "schema"
  kVersion,       // wrong or missing "schema_version"
  kMissingField,  // a required key is absent
  kBadField,      // a key has the wrong JSON type, an out-of-range value or a malformed name
  kBadHash,       // a sha256 that is not 64 lowercase hex characters
  kBadFormat,     // a format other than "zstd-frame"
  kTooMany,       // more entries than Limits::max_entries
  kNoLimits,      // the limits are not ok: nothing is read
  kNotZstd,       // a manifest frame that does not start with the zstd magic (plain JSON is not accepted: nothing uncompressed is published)
  kNoContentSize, // a manifest frame whose header states no content size: its size cannot be held to the limits before it is decoded
  kDecompress,    // the decoder refused the frame, or it decoded to other than the size its header states
};

const char* ParseStatusName(ParseStatus status) noexcept;

struct ParseResult {
  ParseStatus status = ParseStatus::kOk;
  std::string label;      // the label of the entry that failed (a label name is checked against [a-z0-9_] first)
  std::size_t index = 0;  // the entry's index in that label
};

ParseResult ParseManifest(std::string_view text, const Limits& limits, Manifest* out);

// The manifest as it is published: one zstd frame (`manifest.json.zst`) whose header states the content size. The size is held to
// Limits::manifest_text_bytes, which comes from the device's memory, before anything is decoded; the frame is decoded with the zstd
// library the runtime already links (libzstd, as the friend-name profile reply is) into a buffer of exactly the declared size, and
// the JSON text is parsed as ParseManifest does.
// The most bytes a frame of `declared_bytes` of text can take (zstd's own bound): the cap on what is downloaded or read for a manifest.
std::uint64_t MaxManifestFrameBytes(std::uint64_t declared_bytes) noexcept;

ParseResult ParseManifestFrame(std::string_view frame, const Limits& limits, Manifest* out);

// "0x" followed by 1 to 16 hex digits.
bool ParseHex64(std::string_view text, std::uint64_t* out) noexcept;
bool IsSha256Hex(std::string_view text) noexcept;

}  // namespace nevr_cargo

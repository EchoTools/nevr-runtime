/* The CDN manifest: the JSON document, published as one zstd frame, that is on the CDN that lists the game's objects a client may fetch
 * (docs/adr/0045-cdn-texture-delivery.md). One document, `v2/manifest.json.zst`, or `v1/manifest.json.zst` when the CDN has no v2 key (or the manifest a config names, whatever its name:
 * ParseManifestBody reads a zstd frame or plain JSON by its bytes), keyed by label; every
 * entry is one on-disk record of one resource, of any on-disk type, named the way the game names it.
 *
 *   {"schema": "nevr-cargo-manifest", "schema_version": 1,
 *    "labels": {"game": {"entries": [
 *       {"platform": "android-arm64", "type": "0xe2efe7289d5985b8", "name": "0x3fa6d25bc24a7b01", "format": "zstd-frame",
 *        "sha256": "<64 hex of the stored frame>", "size": <frame bytes>, "bytes": <decompressed bytes>, "align": <alignment>,
 *        "stock_size": <optional: the stock buffer the art was made against>}, ...],
 *       "empty": [{"platform": "windows-x86_64", "type": "0x...", "name": "0x...", "align": <alignment>}, ...]}},
 *    "empty": []}
 *
 * A label may also list `scripts`: [{"name": "0x..", "platform": "..", "stock_source": "0x..", "stock_sha256": "<64 hex>"}, ...], each a
 * script the game loads as <scripts>\<name>.dll, served from a copy of the player's own stock script `stock_source` made on the
 * device. No script byte travels. A `scripts` entry that breaks a rule (cdn_validate.h: rekey_edits, image_entry, stock_hash,
 * bad_script, duplicate) refuses its label whole: the label, its entries and its records of 0 bytes are left out and the refusal is
 * listed in Manifest::refused. A manifest without `scripts` parses as before.
 *
 * `type` is the on-disk type, `name` the resource's symbol, both as "0x" hex strings (a JSON number cannot hold 64 bits). `size` is the
 * stored zstd frame, `bytes` what the engine decompresses it to, `align` the record's alignment. The runtime never decompresses: the
 * SHA-256 proves the frame is the published one and the engine decompresses it. `empty` lists the records of 0 bytes, which have no
 * frame. A label's own `empty` list holds the records of that label, each with the platform it belongs to (required): they mount
 * only when the label is on, the platform is the one the plan is made for, and the label mounted an object (cdn_store.h). The
 * top-level `empty` is the older shape: it belongs to the one label of a manifest that has exactly one, on any platform of it; in a
 * manifest with two or more labels its records are refused as `unscoped_empty` (cdn_validate.h).
 *
 * The parser reads with nlohmann::json's non-throwing calls only (the sentinel is built without exceptions). A document that is not
 * exactly this shape is refused whole. An entry the validator refuses (cdn_validate.h) is left out and listed, by rule, in
 * Manifest::refused: one bad record does not take the other 70,000 with it. No size here is a constant: the text, the entry count and
 * every object's size are held to the Limits computed from the device.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cdn/cdn_format.h"
#include "cdn/cdn_limits.h"
#include "cdn/cdn_validate.h"

namespace nevr_cdn {

inline constexpr const char* kSchema = "nevr-cargo-manifest";
inline constexpr int kSchemaVersion = 1;
inline constexpr const char* kFormatZstdFrame = "zstd-frame";

// A label is lowercase letters, digits and underscores, at least one: the name is logged, so its characters are checked before it is
// used. No length is fixed by the format (the game names a resource by a 64-bit symbol, a label is ours); the manifest text limit,
// which comes from the device (cdn_limits.h), bounds it in a manifest, and a script owner's bound (core/map_script_check.h) bounds it
// there. Inline, so the pure owner rule (core/map_script_check.h) needs no cdn_manifest.cpp.
inline bool IsLabelName(std::string_view s) noexcept {
  if (s.empty()) return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

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
  std::string label;     // the label the record belongs to: the key of the label's `empty` list, or the one label of a top-level list
  std::string platform;  // the platform it belongs to; "" for a top-level record (which belongs to the plan's platform)
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  std::uint32_t align = 0;
};

// One entry of a label's `scripts` list, as accepted: the copy `name` of the player's stock script `stock_source` (whose SHA-256 is
// `stock_sha256`), for `platform`.
struct ScriptEntry {
  std::string label;
  std::string platform;
  std::uint64_t name = 0;
  std::uint64_t stock_source = 0;
  std::string stock_sha256;  // 64 lowercase hex characters
};

// An entry the validator left out. A refusal of a `scripts` rule (IsScriptRule) names the offending script entry (type 0, name 0
// when the entry did not give one) and means the whole label is left out.
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
  std::vector<ScriptEntry> scripts;  // in document order, of the labels that were not refused
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
  kNotZstd,       // a frame whose first bytes are not a zstd frame header: no magic (ParseManifestFrame only), or the magic with a
                  // header no frame may have
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

// True when `body` begins with the zstd frame magic (28 B5 2F FD).
bool StartsWithZstdMagic(std::string_view body) noexcept;

// A manifest as it was fetched or saved, decoded by what its bytes are and never by its name: a body that begins with the zstd
// frame magic is one frame (ParseManifestFrame); any other body is the JSON text itself (ParseManifest, held to
// Limits::manifest_text_bytes). Both runtime reads of a manifest go through here: the fetch and the boot's saved copy (cdn_run.cpp).
ParseResult ParseManifestBody(std::string_view body, const Limits& limits, Manifest* out);

// ParseHex64 and IsSha256Hex are cdn_format.h's.

}  // namespace nevr_cdn

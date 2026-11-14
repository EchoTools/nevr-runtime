/* A guild's signed custom-map manifest and a map's package index (docs/adr/0048-custom-maps-channel.md). Both parsers are strict:
 * a document that is not exactly the published shape is refused with a reason token, never with text from the document. Unknown
 * keys are ignored and unknown platforms are skipped. The formats' numbers and names are map_format.h's.
 *
 * Trust: the minisign signature over the manifest body is checked before any JSON is read (ADR 0048 decision 1). Everything else is
 * reached by sha256 from the manifest (manifest -> index -> member -> piece). Each guild has its own manifest, and its body names the
 * guild (`guild`, the group id); comparing that with the guild the manifest was fetched for is the fetch's job. Freshness (ADR 0048
 * decision 3) is kept per signed channel name, guild and signing key: a serial floor (with the body sha256 at that serial) and one
 * version floor per "<id>/<platform>".
 *
 * Manifest reasons: no_memory, too_large, malformed, unsupported_signature, unknown_key, bad_signature, bad_global_signature,
 * bad_schema, channel_missing, bad_channel, wrong_channel, guild_missing, bad_guild, serial_down, serial_reuse, bad_id, bad_version,
 * bad_level, bad_hash, bad_now. A listed entry below its version floor is dropped with version_down (ParsedManifest::dropped); the
 * rest of the manifest, revocations included, is accepted. An expired manifest is accepted: expiry is per map (IsExpired; a map with
 * scripts is not mounted past `expires`, a script-free map still is).
 *
 * Index reasons: no_memory, too_large, index_pin, malformed, bad_schema, entry_mismatch, source_missing, bad_hash, bad_member,
 * bad_path, bad_pieces, set_count, dup_script, rekey_edit, recipe_ops, recipe_bounds, recipe_cap, recipe_tiny, derived_dup,
 * derived_stock_key, bad_recipe_member, alias_count, bad_alias, and recipe_from_forbidden (a field or rekey edit carries a `from`,
 * which would be stock bytes). Recipes are checked for structure only; the rebuild checks them against the player's stock bytes.
 *
 * Memory (ADR 0045 decision 8): a document is read whole, so each parser takes `document_bytes`, the most text this device may hold
 * for one (MapDocumentBytes); the v1 documents' own limits (kMaxManifestBytes, kMaxIndexBytes) apply as well.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/cargo_env.h"
#include "core/map_format.h"
#include "core/map_types.h"
#include "core/minisign_verify.h"

namespace nevr_maps {

struct PinnedKey {
  nevr_minisign::PublicKey key;
  bool official = false;
};

// The floors one signing key raised for one guild on one SIGNED channel name. An empty serial_sha256 is "no floor"; serials start
// at 1.
struct Floors {
  std::uint64_t serial = 0;
  std::string serial_sha256;                    // sha256 of the manifest body at `serial`
  std::map<std::string, std::string> version;   // "<id>/<platform>" -> version
};

// A manifest meets the highest floors of the keys trusted NOW for its guild. A rollback under any trusted key is refused, and a key
// that is no longer trusted (a test key, after the switch to the official key) holds nobody back. Each guild's manifest has its own
// serials, so one guild's floors never hold another back.
struct FloorBook {
  // channel -> guild -> signing key id (KeyIdText) -> floors
  std::map<std::string, std::map<std::string, std::map<std::string, Floors>>> channels;
  std::map<std::string, std::string> bound;  // base_url -> the channel it was bound to at its first verified fetch
};

// `signed_object` is a guild's manifest object: the minisig text, a line "--", then the body.
bool SplitSigned(std::string_view signed_object, std::string_view* minisig, std::string_view* body);

struct ParsedManifest {
  Manifest manifest;
  std::string key_id, body_sha256, channel, guild;  // key_id as minisign prints it: 16 uppercase hex digits
  bool official = false;
  std::string reason;                        // "" when accepted
  std::vector<std::pair<std::string, std::string>> dropped;  // ("<id>/<platform>", "version_down"); absent from `manifest`
  std::vector<std::string> untrusted_floor_keys;  // key ids with floors for this guild that the keys given do not trust
};

// The most bytes of one JSON document (a manifest body, a package index, the cache index) this device may read whole: a share of the
// memory it reports, by the arithmetic the texture cargo uses for its manifest (cargo_limits.h: the text, its copy and its parsed
// form take at most kMemoryShareForManifest of it). 0 when the device gave no memory fact.
std::uint64_t MapDocumentBytes(const nevr_cargo::DeviceFacts& facts) noexcept;

// `expected_channel`: maps.channel, else floors.bound[base_url], else "" (first use: any channel; the caller binds it).
ParsedManifest ParseAndVerifyManifest(std::string_view signed_object, const std::vector<PinnedKey>& keys,
                                      const FloorBook& floors, const std::string& expected_channel,
                                      const std::string& now_utc, std::uint64_t document_bytes);

// The floors after accepting `parsed`, under the key that signed it, for its guild: serial and body sha set, versions raised and
// never lowered, bound[base_url] set. A refused `parsed` (non-empty reason) leaves the floors as they were.
FloorBook AdvanceFloors(const FloorBook& floors, const std::string& base_url, const ParsedManifest& parsed);

struct ParsedPackage {
  Package package;
  std::string reason;  // "" when accepted
};

// How ParsePackageIndex holds `bytes` to the pin `entry.index`. kHash hashes them: the fetch, and a worker's re-hash. kStamped hashes
// nothing: the caller read the bytes through a handle whose file stamp equals the one the cache index recorded when the bytes were
// hashed at fetch (ADR 0048 decision 4). The mount, which may run on the boot thread, uses kStamped, so it hashes nothing there
// (ADR 0045 decision 7). Both check the size.
enum class IndexPin { kHash, kStamped };

// `bytes` must be the blob `entry.index` pins: its size is checked here, and with IndexPin::kHash its sha256 too (index_pin).
ParsedPackage ParsePackageIndex(std::string_view bytes, const std::string& id, const std::string& platform,
                                const PlatformEntry& entry, std::uint64_t document_bytes, IndexPin pin = IndexPin::kHash);

// Dotted numerics, 1 to 4 parts of 1 to 9 digits with no leading zero ("1.2.0"); a missing part is 0. An invalid version
// is below every valid one. Returns <0, 0 or >0.
int CompareVersions(std::string_view a, std::string_view b);
bool IsValidVersion(std::string_view version);
bool IsValidMapId(std::string_view id);        // [a-z][a-z0-9_]{0,54}
bool IsValidChannel(std::string_view channel); // [a-z0-9][a-z0-9.-]{0,63}
// A lobby group id held as an opaque string: 1 to kGroupIdMaxBytes characters of [0-9a-z-], safe as a URL path segment and a
// directory name. Never parsed as a UUID.
bool IsPathSafeGroupId(std::string_view group);
bool IsValidUtc(std::string_view text);        // YYYY-MM-DDTHH:MM:SSZ, a real calendar date

// True when `now_utc` is at or past `expires`, and also when either is not valid UTC (fails closed).
bool IsExpired(const std::string& expires, const std::string& now_utc);

// A minisign key id as minisign prints it: 16 uppercase hex digits of the little-endian number.
std::string KeyIdText(const std::array<std::uint8_t, kMinisignKeyIdBytes>& key_id);

}  // namespace nevr_maps

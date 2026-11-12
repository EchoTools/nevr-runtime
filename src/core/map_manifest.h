/* The custom-map channel's signed manifest and a map's package index (docs/adr/0047-custom-maps-channel.md,
 * docs/reference/maps-channel.md). Both parsers are strict: a document that is not exactly the published shape is refused
 * with a reason token, never with text from the document. Unknown keys are ignored and unknown platforms are skipped.
 *
 * Trust: the minisign signature over the manifest body is checked before any JSON is read. Everything else is reached by
 * sha256 from the manifest (manifest -> index -> member -> piece). Freshness is kept per SIGNED channel name: a serial floor
 * (with the body sha256 at that serial) and one version floor per "<id>/<platform>".
 *
 * Manifest reasons: too_large, malformed, unsupported_signature, unknown_key, bad_signature, bad_global_signature, bad_schema,
 * channel_missing, bad_channel, wrong_channel, serial_down, serial_reuse, bad_id, bad_version, bad_level, bad_hash, bad_now.
 * A listed entry below its version floor is dropped with version_down (ParsedManifest::dropped); the rest of the manifest,
 * revocations included, is accepted. An expired manifest is accepted: expiry is per map (IsExpired; a map with scripts is
 * not mounted past `expires`, a script-free map still is).
 *
 * Index reasons: too_large, index_pin, malformed, bad_schema, entry_mismatch, source_missing, bad_hash, bad_member,
 * bad_path, bad_pieces, set_count, dup_script, rekey_edit, recipe_ops, recipe_bounds, recipe_cap, recipe_tiny, derived_dup,
 * derived_stock_key, bad_recipe_member, alias_count, bad_alias, and recipe_from_forbidden (a field or rekey edit carries a
 * `from`, which would be stock bytes). Recipes are checked for structure only; PC10 checks them against the player's stock
 * bytes.
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

#include "core/map_types.h"
#include "core/minisign_verify.h"

namespace nevr_maps {

struct PinnedKey {
  nevr_minisign::PublicKey key;
  bool official = false;
};

// One floor per SIGNED channel name, over every key it trusts. An empty serial_sha256 is "no floor"; serials start at 1.
struct Floors {
  std::uint64_t serial = 0;
  std::string serial_sha256;                    // sha256 of the manifest body at `serial`
  std::map<std::string, std::string> version;   // "<id>/<platform>" -> version
};

struct FloorBook {
  std::map<std::string, Floors> channels;       // channel -> its floors
  std::map<std::string, std::string> bound;     // base_url -> the channel it was bound to at its first verified fetch
};

// `signed_object` is v1/maps/manifest.signed: the minisig text, a line "--", then the body.
bool SplitSigned(std::string_view signed_object, std::string_view* minisig, std::string_view* body);

struct ParsedManifest {
  Manifest manifest;
  std::string key_id, body_sha256, channel;  // key_id as minisign prints it: 16 uppercase hex digits
  bool official = false;
  std::string reason;                        // "" when accepted
  std::vector<std::pair<std::string, std::string>> dropped;  // ("<id>/<platform>", "version_down"); absent from `manifest`
};

// `expected_channel`: maps.channel, else floors.bound[base_url], else "" (first use: any channel; the caller binds it).
ParsedManifest ParseAndVerifyManifest(std::string_view signed_object, const std::vector<PinnedKey>& keys,
                                      const FloorBook& floors, const std::string& expected_channel,
                                      const std::string& now_utc);

// The floors after accepting `parsed`: serial and body sha set, versions raised and never lowered, bound[base_url] set.
// A refused `parsed` (non-empty reason) leaves the floors as they were.
FloorBook AdvanceFloors(const FloorBook& floors, const std::string& base_url, const ParsedManifest& parsed);

struct ParsedPackage {
  Package package;
  std::string reason;  // "" when accepted
};

// `bytes` must be the blob `entry.index` pins (size and sha256 are checked here too).
ParsedPackage ParsePackageIndex(std::string_view bytes, const std::string& id, const std::string& platform,
                                const PlatformEntry& entry);

// Dotted numerics, 1 to 4 parts of 1 to 9 digits with no leading zero ("1.2.0"); a missing part is 0. An invalid version
// is below every valid one. Returns <0, 0 or >0.
int CompareVersions(std::string_view a, std::string_view b);
bool IsValidVersion(std::string_view version);
bool IsValidMapId(std::string_view id);        // [a-z][a-z0-9_]{0,54}
bool IsValidChannel(std::string_view channel); // [a-z0-9][a-z0-9.-]{0,63}
bool IsValidUtc(std::string_view text);        // YYYY-MM-DDTHH:MM:SSZ, a real calendar date

// True when `now_utc` is at or past `expires`, and also when either is not valid UTC (fails closed).
bool IsExpired(const std::string& expires, const std::string& now_utc);

// A minisign key id as minisign prints it: 16 uppercase hex digits of the little-endian number.
std::string KeyIdText(const std::array<std::uint8_t, 8>& key_id);

constexpr std::size_t kMaxManifestBytes = 4u << 20, kMaxMinisigBytes = 4096, kMaxIndexBytes = 8u << 20;
constexpr std::uint64_t kMaxPieceBytes = 128ull << 20;
constexpr std::size_t kMaxConfigAliases = 4;

}  // namespace nevr_maps

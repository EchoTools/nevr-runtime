// map_script_check.h: the decisions a map's script goes through before it is held (docs/adr/0048 decisions 6, 7, 10 and 15).
// One copy for both platforms: the PC provider (runtime/content/script_provider.cpp) and the Quest table (quest/plugin/
// script_redirect.h) read their file and keep their table, and ask this for every decision. The image rules stay in
// core/script_image_rules.h (one rule set per format); its exported token list names every refusal.
//
// Pure: no Win32, no POSIX, no Log(). A token is a string literal, so a caller that keeps a `const char*` needs no storage.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/cargo_format.h"
#include "core/map_manifest.h"
#include "core/resource_set_registry.h"
#include "core/script_image_rules.h"

namespace nevr_map_script {

// True when `text` is a sha-256 digest in 64 lowercase hex characters.
inline bool IsSha256Digest(const std::string& text) {
  if (text.size() != nevr_cargo::kSha256HexChars) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// The owner of a map's scripts: the registry's map owner (its prefix, at most kMaxMapOwnerBytes bytes) and a valid map id
// after the prefix (nevr_maps::IsValidMapId). The registry checks the prefix and the length of a set's owner; the id rule is
// checked here, so a script is never held for an owner the channel would refuse.
inline bool IsMapScriptOwner(const std::string& owner) {
  if (!nevr_resource_sets::IsMapOwner(owner) || owner.size() > nevr_resource_sets::kMaxMapOwnerBytes) return false;
  const std::string_view id = std::string_view(owner).substr(sizeof(nevr_resource_sets::kMapOwnerPrefix) - 1);
  return nevr_maps::IsValidMapId(id);
}

// The argument refusals, in order: bad_owner, bad_hash (0 names no script), bad_sha256. nullptr when well formed.
inline const char* ArgumentRefusal(const std::string& owner, std::uint64_t hash, const std::string& sha256) {
  if (!IsMapScriptOwner(owner)) return "bad_owner";
  if (hash == 0) return "bad_hash";
  if (!IsSha256Digest(sha256)) return "bad_sha256";
  return nullptr;
}

// bad_size when the file is empty or larger than `byte_limit` (the device's share for a document, MapDocumentBytes), or when
// the package index pins its size (`pinned_bytes`, 0 when nothing pins it) and the file is not that size. A negative size from
// the OS arrives as a very large unsigned value and is refused by the limit. nullptr otherwise.
inline const char* SizeRefusal(std::uint64_t size, std::uint64_t pinned_bytes, std::uint64_t byte_limit) {
  if (size == 0 || size > byte_limit) return "bad_size";
  if (pinned_bytes != 0 && size != pinned_bytes) return "bad_size";
  return nullptr;
}

// sha_mismatch when the bytes' digest `actual` is not the manifest's `expected`. nullptr when they are equal.
inline const char* DigestRefusal(const std::string& actual, const std::string& expected) {
  return actual == expected ? nullptr : "sha_mismatch";
}

// The facts a claim is refused on, each read by the platform.
struct ClaimFacts {
  bool withdrawn = false;    // the owner was withdrawn in this process (its later registrations are refused)
  bool signed_only = false;  // the run is -require-signed-plugins and the manifest that pins the script is not official
                             // (decision 7: core map_run's refusal, the same fact; no config key makes a manifest official)
  bool revoked = false;      // the file's digest is on the manifest's revocation list (decision 10)
  bool duplicate = false;    // another live script holds the hash
  bool stock = false;        // the game has its own file for the hash (the stock redirect, or the player's install)
  bool pack = false;         // the content pack carries the hash
};

// The first refusal in the order withdrawn, signed_only, revoked, duplicate, stock_hash, pack_hash; nullptr when none holds.
inline const char* ClaimRefusal(const ClaimFacts& facts) {
  if (facts.withdrawn) return "withdrawn";
  if (facts.signed_only) return "signed_only";
  if (facts.revoked) return "revoked";
  if (facts.duplicate) return "duplicate";
  if (facts.stock) return "stock_hash";
  if (facts.pack) return "pack_hash";
  return nullptr;
}

// The token a refusal of an image rule reports: the rules' own vocabulary (core/script_image_rules.h kImageRuleTokens), or
// image_rejected for a reason the vocabulary does not hold.
inline const char* ImageRefusalToken(const std::string& reason) {
  for (const char* token : nevr_script_image::kImageRuleTokens) {
    if (reason == token) return token;
  }
  return "image_rejected";
}

}  // namespace nevr_map_script

// map_script_check.h: the decisions a map's script goes through before it is held (docs/adr/0048 decisions 6, 7 and 10). One
// copy for both platforms: the PC provider (runtime/content/script_provider.cpp) and the Quest table (quest/plugin/
// script_redirect.h) read their file and keep their table, and ask this for every decision. The image rules stay in
// core/script_image_rules.h (one rule set per format); this header names the token each of their refusals reports.
//
// Pure: no Win32, no POSIX, no Log(). A token is a string literal, so a caller that keeps a `const char*` needs no storage.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/cargo_format.h"

namespace nevr_map_script {

// A map's scripts are owned by "map:" and the map's id (the resource-set owner the channel registers them under).
inline constexpr char kOwnerPrefix[] = "map:";

// The largest script file a claim reads. Stock script DLLs are tens of KiB, so this is a sanity bound on the file's size, not a
// value of the format and not one the device gives; it is not yet derived from the device as ADR 0045
// decision 8 would have the channel's limits be.
inline constexpr std::uint64_t kMaxScriptBytes = 64ull << 20;

// True when `text` is a sha-256 digest in 64 lowercase hex characters.
inline bool IsSha256Digest(const std::string& text) {
  if (text.size() != nevr_cargo::kSha256HexChars) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// The argument refusals, in order: bad_owner (not "map:" followed by an id), bad_hash (0), bad_sha256 (not a digest). nullptr
// when the arguments are well formed.
inline const char* ArgumentRefusal(const std::string& owner, std::uint64_t hash, const std::string& sha256) {
  const std::size_t prefix = sizeof kOwnerPrefix - 1;
  if (owner.size() <= prefix || owner.compare(0, prefix, kOwnerPrefix) != 0) return "bad_owner";
  if (hash == 0) return "bad_hash";
  if (!IsSha256Digest(sha256)) return "bad_sha256";
  return nullptr;
}

// bad_size when the file is empty or larger than kMaxScriptBytes (a negative size from the OS arrives as a very large unsigned
// value and is refused here too). nullptr otherwise.
inline const char* SizeRefusal(std::uint64_t size) {
  return (size == 0 || size > kMaxScriptBytes) ? "bad_size" : nullptr;
}

// sha_mismatch when the bytes' digest `actual` is not the manifest's `expected`. nullptr when they are equal.
inline const char* DigestRefusal(const std::string& actual, const std::string& expected) {
  return actual == expected ? nullptr : "sha_mismatch";
}

// The facts a claim is refused on, each read by the platform.
struct ClaimFacts {
  bool withdrawn = false;    // the owner was withdrawn in this process (its later registrations are refused)
  bool signed_only = false;  // the run is -require-signed-plugins and no official key counts (decision 7)
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

// The token a refusal of an image rule reports (core/script_image_rules.h names each one). Every rule token maps to itself;
// anything else is image_rejected.
inline const char* ImageRefusalToken(const std::string& reason) {
  static constexpr const char* kTokens[] = {
      "pe_truncated",        "pe_not_pe",           "pe_not_amd64",       "pe_not_pe32plus",     "pe_not_dll",
      "pe_imports",          "pe_delay_imports",    "pe_bound_imports",   "pe_iat",              "pe_tls",
      "pe_clr",              "pe_entry_point",      "pe_no_exports",      "pe_export_count",     "pe_export_name",
      "pe_export_forwarded", "pe_export_not_code",  "pe_section_name",    "pe_section_wx",       "elf_truncated",
      "elf_not_elf",         "elf_not_64",          "elf_not_le",         "elf_not_dyn",         "elf_not_aarch64",
      "elf_segment_wx",      "elf_no_dynamic",      "elf_textrel",        "elf_no_symbols",      "elf_needed",
      "elf_init",            "elf_no_export",       "elf_export_not_code"};
  for (const char* token : kTokens) {
    if (reason == token) return token;
  }
  return "image_rejected";
}

}  // namespace nevr_map_script

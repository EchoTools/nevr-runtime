/* The client's boot-fetch decision (PLAN-v7 5.4 "Client start" item 1 and "The boot-fetch stamp"; ADR 0045 decision 7 says "never block
 * the boot" and is amended for this by R6b-2, PLAN-v7 11.3): whether a client fetches its boot-only labels before the game reads its
 * first resource. Pure: it reads no file, makes no request and writes no log. The caller (cdn_run.cpp, from R6b-2 on) fills the input
 * from the saved manifest, the store index, the marker file, the stamp file and the run's own facts, then acts on the answer.
 *
 * The boot-fetch stamp is one JSON object beside the saved manifest, written after a boot fetch or a background fetch ends in a cause a
 * retry of the same manifest cannot cure (settled, undeliverable, no_space). Only NeedsBootFetch reads it.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"

namespace nevr_cdn {

// What the stamp says about the saved manifest: every pin object held (settled), a pin object the CDN cannot give (undeliverable), or
// no room for the pin labels' objects (no_space).
enum class BootFetchCause : std::uint8_t { kSettled, kUndeliverable, kNoSpace };
const char* BootFetchCauseName(BootFetchCause cause) noexcept;  // "settled", "undeliverable", "no_space"
// The cause named by `text` (exactly one of the three names); false for anything else.
bool ParseBootFetchCause(const std::string& text, BootFetchCause* out) noexcept;

struct BootFetchStamp {
  std::string manifest_sha256;  // 64 lowercase hex characters: the sha256 of the saved manifest's bytes
  BootFetchCause cause = BootFetchCause::kSettled;
  std::uint64_t free_bytes = 0;  // no_space: the free space when the boot fetch stopped; 0 otherwise
  std::uint64_t need_bytes = 0;  // no_space: the pin labels' bytes still to fetch then; 0 otherwise
};

// The stamp's text: one JSON object with the four keys. Written with nlohmann::json::dump.
std::string FormatBootFetchStamp(const BootFetchStamp& stamp);
// Parses a stamp with the non-throwing parser. False, and `out` untouched, when the text is not a JSON object with the four keys, the
// hash is not 64 lowercase hex characters, the cause is not one of the three, or a byte count is not a non-negative integer. A false
// stamp is absent: NeedsBootFetch treats it as no stamp.
bool ParseBootFetchStamp(const std::string& text, BootFetchStamp* out);

// Why a boot fetch is needed, in the order NeedsBootFetch checks them.
enum class BootFetchReason : std::uint8_t { kNone, kNoSavedManifest, kRefetchMarker, kLabelAbsent, kObjectsMissing };
const char* BootFetchReasonName(BootFetchReason reason) noexcept;  // "none", "no_saved_manifest", "refetch_marker", "label_absent", "objects_missing"

struct BootFetchInput {
  // The saved manifest, parsed; null when there is none. `saved_sha256` is the sha256 of its bytes (empty when there is none).
  const Manifest* saved = nullptr;
  std::string saved_sha256;
  Platform platform = kAndroidArm64;
  Policy policy;
  // The labels fixed at boot (RunConfig::boot_only). Empty: no boot fetch (the Quest, and a run with nothing boot-only).
  std::vector<std::string> boot_only;
  // The refetch marker (`<cache>\cdn\boot_refetch`) exists.
  bool marker = false;
  // The parsed boot-fetch stamp; null when absent or not parsed.
  const BootFetchStamp* stamp = nullptr;
  // Whether the store holds an object of this entry, with its size (Store::HoldsSized): a lookup, never a read or a hash. Unset: no
  // object is held, so every pin label is refetched.
  std::function<bool(const Entry&)> held;
  // The free space of the cache directory, read only when the stamp is a no_space stamp for the saved manifest. It returns false when the
  // space cannot be read, and then the stamp does not settle the manifest. Unset: the same.
  std::function<bool(std::uint64_t* free_bytes)> free_space;
  // A signed-only run whose manifest is not verified (cdn_scripts.h ManifestVerified): the run refuses those labels, so it fetches none.
  bool signed_only_refuses = false;
  // This start is the last start of a self-update trial (PLAN-v7 G9; set by the PC edge in R6b-3).
  bool last_update_trial = false;
};

struct BootFetchDecision {
  bool fetch = false;                           // a boot fetch is made before the boot mount
  BootFetchReason reason = BootFetchReason::kNone;
  std::string label;                            // the pin label that asked for it; empty when fetch is false
};

// The answer for one client start. Yes only when every condition of PLAN-v7 5.4 item 1 holds: `boot_only` is not empty; neither
// `signed_only_refuses` nor `last_update_trial` is set; and for a pin label (nevr_cargo_pin::kCargoPinLabels) that is on or required
// in the policy (required_only keeps only the required ones), one of, checked in this order:
//   no saved manifest; the refetch marker; the saved manifest holds no record of the label for this platform (label_absent) or names
//   an object of it the store does not hold (objects_missing), unless the stamp settles the saved manifest.
// The stamp settles it when its hash equals `saved_sha256` and its cause is settled or undeliverable, or its cause is no_space and the free
// space is less than its free_bytes plus its need_bytes (saturating at the largest value).
BootFetchDecision NeedsBootFetch(const BootFetchInput& input);

}  // namespace nevr_cdn

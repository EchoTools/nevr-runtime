/* What a client fetches from a CDN manifest (docs/adr/0045-cdn-texture-delivery.md, D5 and D6): the labels whose state is on, the
 * entries of this platform, the size rules, all decided before any request is made.
 *
 * State of a label. Code holds the defaults (kLabelDefaults) and what an unlisted label is (kUnlistedLabelState); config only
 * changes them (`assets.labels.<label>: on|off|required`, `assets.required_only`). The manifest never sets one.
 *   off       not fetched, not served, its blobs unreferenced
 *   on        fetched in the background, served when mounted; a failure is a Warning and the stock asset shows
 *   required  fetched first, and also under required_only; a failure is an Error. It never blocks a level load or a match.
 *
 * Budgets. A label is planned whole or skipped whole: its blobs that the store does not hold must fit the storage budget, and the
 * store files they need must fit the descriptors (cdn_limits.h). Both numbers come from the device.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cdn/cdn_limits.h"
#include "cdn/cdn_manifest.h"

namespace nevr_cdn {

enum class LabelState : std::uint8_t { kOff, kOn, kRequired };
const char* LabelStateName(LabelState state) noexcept;
// "on", "off" or "required" (case-sensitive); false for anything else.
bool ParseLabelState(std::string_view text, LabelState* out) noexcept;

struct LabelDefault {
  const char* label;
  LabelState state;
};
// `game` is the whole-game label of an all-assets manifest: it is the content the game needs to run, so it is required.
inline constexpr LabelDefault kLabelDefaults[] = {{"lobby_posters", LabelState::kOn}, {"game", LabelState::kRequired}};
inline constexpr LabelState kUnlistedLabelState = LabelState::kOff;

struct Policy {
  std::map<std::string, LabelState> overrides;  // from config
  bool required_only = false;
};

LabelState StateOf(const std::string& label, const Policy& policy);

enum class SkipReason : std::uint8_t {
  kNone = 0,
  kOff,           // the label's state is off
  kRequiredOnly,  // assets.required_only and the label is not required
  kNoEntries,     // the manifest has none of this platform's entries under it
  kOverStorage,   // the blobs to fetch do not fit what is left of Limits::storage_budget
  kOverFiles,     // the blobs would need more store files than Limits::max_store_files
};
const char* SkipReasonName(SkipReason reason) noexcept;

struct PlanItem {
  Entry entry;
  bool cached = false;  // the store already holds the blob: no request, no hashing
};

// A resource name dropped whole: a payload whose size differs from the stock buffer's, without the descriptor it needs.
struct PairRefusal {
  std::uint64_t name = 0;
  const char* reason = "unpaired_payload";
};

struct PlanLabel {
  std::string label;
  LabelState state = LabelState::kOff;
  SkipReason skipped = SkipReason::kNone;
  std::size_t entries = 0;   // kept entries of this platform
  std::size_t cached = 0;    // unique blobs already held
  std::size_t to_fetch = 0;  // unique blobs to request
  std::uint64_t fetch_bytes = 0;
  std::vector<PlanItem> items;         // empty when skipped
  std::vector<PairRefusal> refused;    // resource names dropped whole
};

struct Plan {
  std::vector<PlanLabel> labels;  // required labels first, then the others, each group in document order
  std::size_t platform_entries = 0;  // entries of this platform in the whole manifest
  std::uint64_t fetch_bytes = 0;     // bytes to request over every planned label
  std::size_t to_fetch = 0;          // blobs to request over every planned label
};

// `cached(sha256)` says whether the store holds a blob of that name. It is a lookup, never a hash: a blob is hashed once, when it is
// downloaded. `held_bytes` is what the store holds now, for the file count.
Plan MakePlan(const Manifest& manifest, const Platform& platform, const Policy& policy, const Limits& limits,
              std::uint64_t held_bytes, const std::function<bool(const Entry&)>& held);

}  // namespace nevr_cdn

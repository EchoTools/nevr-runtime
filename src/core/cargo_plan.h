/* What a client fetches from a cargo manifest (docs/adr/0045-cdn-texture-delivery.md, D5 and D6): the labels whose state is on, the
 * entries of this platform, the size rules, all decided before any request is made.
 *
 * State of a label. Code holds the defaults (kLabelDefaults) and what an unlisted label is (kUnlistedLabelState); config only
 * changes them (`assets.labels.<label>: on|off|required`, `assets.required_only`). The manifest never sets one.
 *   off       not fetched, not served, its blobs unreferenced
 *   on        fetched in the background, served when mounted; a failure is a Warning and the stock texture shows
 *   required  fetched first, and also under required_only; a failure is an Error. It never blocks a level load or a match.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/cargo_manifest.h"

namespace nevr_cargo {

enum class LabelState : std::uint8_t { kOff, kOn, kRequired };
const char* LabelStateName(LabelState state) noexcept;
// "on", "off" or "required" (case-sensitive); false for anything else.
bool ParseLabelState(std::string_view text, LabelState* out) noexcept;

struct LabelDefault {
  const char* label;
  LabelState state;
};
inline constexpr LabelDefault kLabelDefaults[] = {{"lobby_posters", LabelState::kOn}};
inline constexpr LabelState kUnlistedLabelState = LabelState::kOff;

struct Policy {
  std::map<std::string, LabelState> overrides;  // from config
  bool required_only = false;
};

LabelState StateOf(const std::string& label, const Policy& policy);

enum class SkipReason : std::uint8_t {
  kNone = 0,
  kOff,            // the label's state is off
  kRequiredOnly,   // assets.required_only and the label is not required
  kNoEntries,      // the manifest has none of this platform's entries under it
  kOverCap,        // the label's blobs do not fit what is left of kDefaultCapBytes
};
const char* SkipReasonName(SkipReason reason) noexcept;

struct PlanItem {
  Entry entry;
  bool cached = false;  // a blob that already re-hashes to its name needs no request
};

struct Refusal {
  std::uint64_t name = 0;
  const char* reason = "unpaired_payload";
};

struct PlanLabel {
  std::string label;
  LabelState state = LabelState::kOff;
  SkipReason skipped = SkipReason::kNone;
  std::size_t entries = 0;     // kept entries of this platform
  std::size_t cached = 0;      // unique blobs already held
  std::size_t to_fetch = 0;    // unique blobs to request
  std::uint64_t fetch_bytes = 0;
  std::vector<PlanItem> items;      // empty when skipped
  std::vector<Refusal> refused;     // resource names dropped whole (a payload without the descriptor it needs)
};

struct Plan {
  std::vector<PlanLabel> labels;   // required labels first, then the others, each group in document order
  std::size_t platform_entries = 0;  // entries of this platform in the whole manifest
};

// `cached(sha256)` says whether a blob of that name is already held and verified.
Plan MakePlan(const Manifest& manifest, const Platform& platform, const Policy& policy,
              const std::function<bool(const std::string&)>& cached);

}  // namespace nevr_cargo

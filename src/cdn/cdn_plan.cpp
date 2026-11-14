#include "cdn/cdn_plan.h"

#include <map>
#include <set>
#include <vector>

namespace nevr_cdn {

const char* LabelStateName(LabelState state) noexcept {
  switch (state) {
    case LabelState::kOff: return "off";
    case LabelState::kOn: return "on";
    case LabelState::kRequired: return "required";
  }
  return "unknown";
}

bool ParseLabelState(std::string_view text, LabelState* out) noexcept {
  if (text == "on") {
    *out = LabelState::kOn;
  } else if (text == "off") {
    *out = LabelState::kOff;
  } else if (text == "required") {
    *out = LabelState::kRequired;
  } else {
    return false;
  }
  return true;
}

const char* SkipReasonName(SkipReason reason) noexcept {
  switch (reason) {
    case SkipReason::kNone: return "none";
    case SkipReason::kOff: return "off";
    case SkipReason::kRequiredOnly: return "required_only";
    case SkipReason::kNoEntries: return "no_entries";
    case SkipReason::kOverStorage: return "over_storage";
    case SkipReason::kOverFiles: return "over_files";
  }
  return "unknown";
}

LabelState StateOf(const std::string& label, const Policy& policy) {
  const auto it = policy.overrides.find(label);
  if (it != policy.overrides.end()) return it->second;
  for (const LabelDefault& d : kLabelDefaults) {
    if (label == d.label) return d.state;
  }
  return kUnlistedLabelState;
}

Plan MakePlan(const Manifest& manifest, const Platform& platform, const Policy& policy, const Limits& limits,
              std::uint64_t held_bytes, const std::function<bool(const Entry&)>& held) {
  Plan plan;
  for (const Entry& e : manifest.entries) {
    if (e.platform == platform.name) ++plan.platform_entries;
  }
  // Required labels first, then the others, each group in document order.
  std::vector<const std::string*> order;
  for (const std::string& label : manifest.labels) {
    if (StateOf(label, policy) == LabelState::kRequired) order.push_back(&label);
  }
  for (const std::string& label : manifest.labels) {
    if (StateOf(label, policy) != LabelState::kRequired) order.push_back(&label);
  }
  // The entries of this platform by label, once, so a manifest of seventy thousand records is walked once and not once a label.
  std::map<std::string, std::vector<const Entry*>> byLabel;
  for (const Entry& e : manifest.entries) {
    if (e.platform == platform.name) byLabel[e.label].push_back(&e);
  }
  std::set<std::string> counted;  // blobs already charged to the budget by an earlier label (or held: those cost nothing)
  std::uint64_t used = 0;
  // The store files, as the store fills them: the held bytes fill whole files and leave a tail free in the last one; a blob is not
  // split across two files, so it goes in the tail when it fits and opens a new file when it does not.
  const std::uint64_t fileBytes = limits.store_file_bytes;
  std::uint64_t files = fileBytes == 0 ? 0 : (held_bytes + fileBytes - 1) / fileBytes;
  std::uint64_t tail = files * fileBytes - held_bytes;
  const std::vector<const Entry*> none;
  for (const std::string* labelPtr : order) {
    const std::string& label = *labelPtr;
    PlanLabel pl;
    pl.label = label;
    pl.state = StateOf(label, policy);
    const auto found = byLabel.find(label);
    const std::vector<const Entry*>& mine = found != byLabel.end() ? found->second : none;
    if (pl.state == LabelState::kOff) {
      pl.skipped = SkipReason::kOff;
    } else if (policy.required_only && pl.state != LabelState::kRequired) {
      pl.skipped = SkipReason::kRequiredOnly;
    } else if (mine.empty()) {
      pl.skipped = SkipReason::kNoEntries;
    } else {
      // A payload whose bytes differ from the stock buffer it was made against carries a different shape, which only its descriptor
      // can state: without the descriptor for the name in this label, the whole name is refused. A payload with no stock size states
      // nothing to compare against.
      std::set<std::uint64_t> refusedNames;
      for (std::size_t p = 0; p < platform.pair_count; ++p) {
        const PairRule& pair = platform.pairs[p];
        std::set<std::uint64_t> hasDescriptor;
        for (const Entry* e : mine) {
          if (e->type == pair.descriptor) hasDescriptor.insert(e->name);
        }
        for (const Entry* e : mine) {
          if (e->type == pair.payload && e->has_stock_size && e->bytes != e->stock_size && hasDescriptor.count(e->name) == 0) {
            refusedNames.insert(e->name);
          }
        }
      }
      for (const std::uint64_t name : refusedNames) {
        PairRefusal r;
        r.name = name;
        pl.refused.push_back(r);
      }
      std::vector<const Entry*> kept;
      for (const Entry* e : mine) {
        if (refusedNames.count(e->name) == 0) kept.push_back(e);
      }
      if (kept.empty()) {
        pl.skipped = SkipReason::kNoEntries;
      } else {
        // What this label adds to the store: each blob it needs that the store does not hold and no earlier label charged.
        std::set<std::string> fresh;
        std::vector<std::uint64_t> freshSizes;
        std::uint64_t freshBytes = 0;
        bool tooBigForAFile = false;
        for (const Entry* e : kept) {
          if (counted.count(e->sha256) != 0 || fresh.count(e->sha256) != 0) continue;
          if (held(*e)) continue;
          fresh.insert(e->sha256);
          freshSizes.push_back(e->size);
          freshBytes += e->size;
          if (e->size > fileBytes) tooBigForAFile = true;
        }
        std::uint64_t nextFiles = files, nextTail = tail;
        for (const std::uint64_t size : freshSizes) {
          if (size <= nextTail) {
            nextTail -= size;
          } else {
            ++nextFiles;
            nextTail = fileBytes - (size <= fileBytes ? size : fileBytes);
          }
        }
        if (used + freshBytes > limits.storage_budget) {
          pl.skipped = SkipReason::kOverStorage;
        } else if (tooBigForAFile || nextFiles > limits.max_store_files) {
          pl.skipped = SkipReason::kOverFiles;
        } else {
          used += freshBytes;
          files = nextFiles;
          tail = nextTail;
          counted.insert(fresh.begin(), fresh.end());
          std::set<std::string> seen;
          for (const Entry* e : kept) {
            PlanItem item;
            item.entry = *e;
            item.cached = held(*e);
            if (seen.insert(e->sha256).second) {
              if (item.cached) {
                ++pl.cached;
              } else if (fresh.count(e->sha256) != 0) {  // a blob an earlier label already asks for is not asked for twice
                ++pl.to_fetch;
                pl.fetch_bytes += e->size;
              }
            }
            pl.items.push_back(std::move(item));
          }
          pl.entries = kept.size();
          plan.fetch_bytes += pl.fetch_bytes;
          plan.to_fetch += pl.to_fetch;
        }
      }
    }
    plan.labels.push_back(std::move(pl));
  }
  return plan;
}

}  // namespace nevr_cdn

#include "core/cargo_plan.h"

#include <set>

namespace nevr_cargo {

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
    case SkipReason::kOverCap: return "over_cap";
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

Plan MakePlan(const Manifest& manifest, const Platform& platform, const Policy& policy,
              const std::function<bool(const std::string&)>& cached) {
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
  std::set<std::string> counted;  // blobs already charged to the cap by an earlier label
  std::uint64_t used = 0;
  for (const std::string* labelPtr : order) {
    const std::string& label = *labelPtr;
    PlanLabel pl;
    pl.label = label;
    pl.state = StateOf(label, policy);
    std::vector<const Entry*> mine;
    for (const Entry& e : manifest.entries) {
      if (e.label == label && e.platform == platform.name) mine.push_back(&e);
    }
    if (pl.state == LabelState::kOff) {
      pl.skipped = SkipReason::kOff;
    } else if (policy.required_only && pl.state != LabelState::kRequired) {
      pl.skipped = SkipReason::kRequiredOnly;
    } else if (mine.empty()) {
      pl.skipped = SkipReason::kNoEntries;
    } else {
      // A buffer-1 entry whose bytes differ from the stock buffer carries a different texture shape, which only buffer 0 (the
      // descriptor) can state: without a buffer-0 entry for the name in this label, the whole name is refused.
      std::set<std::uint64_t> hasDescriptor;
      for (const Entry* e : mine) {
        if (e->type == platform.buffer0_type) hasDescriptor.insert(e->name);
      }
      std::set<std::uint64_t> refusedNames;
      for (const Entry* e : mine) {
        if (e->type == platform.buffer1_type && e->bytes != e->stock_size && hasDescriptor.count(e->name) == 0) {
          refusedNames.insert(e->name);
        }
      }
      for (const std::uint64_t name : refusedNames) {
        Refusal r;
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
        std::set<std::string> fresh;
        std::uint64_t freshBytes = 0;
        for (const Entry* e : kept) {
          if (counted.count(e->sha256) == 0 && fresh.insert(e->sha256).second) freshBytes += e->size;
        }
        if (used + freshBytes > kDefaultCapBytes) {
          pl.skipped = SkipReason::kOverCap;
        } else {
          used += freshBytes;
          counted.insert(fresh.begin(), fresh.end());
          std::set<std::string> seen;
          for (const Entry* e : kept) {
            PlanItem item;
            item.entry = *e;
            item.cached = cached(e->sha256);
            if (seen.insert(e->sha256).second) {
              if (item.cached) {
                ++pl.cached;
              } else {
                ++pl.to_fetch;
                pl.fetch_bytes += e->size;
              }
            }
            pl.items.push_back(std::move(item));
          }
          pl.entries = kept.size();
        }
      }
    }
    plan.labels.push_back(std::move(pl));
  }
  return plan;
}

}  // namespace nevr_cargo

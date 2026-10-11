// What a binding does after an allocation hit a script's memory cap
// (script_vm.h, "Limits"): after collecting, either the call merely failed
// (garbage outran the collector) or the owner is disabled. VM-independent, so
// every binding judges the same way and the rule is tested without a VM.
#pragma once

#include <cstddef>

namespace nevr_script {

// Calls in a row that may reach the cap with garbage before it counts as a breach.
constexpr int kCapHitsInARowLimit = 3;

struct CapHit {
  size_t live_after_collection;  // bytes the state holds after a full collection
  size_t largest_refused;        // the biggest single request the allocator refused
  size_t cap;                    // VmLimits::memory_bytes
  int hits_in_a_row;             // calls in a row that reached the cap, this one included
};

enum class CapVerdict {
  kGarbage,          // only this call failed; the script continues
  kLiveSetTooLarge,  // over half the cap is live
  kRequestTooLarge,  // one request alone was over half the cap
  kRepeated,         // kCapHitsInARowLimit calls in a row at the cap
};

constexpr CapVerdict JudgeCapHit(const CapHit& hit) {
  if (hit.live_after_collection > hit.cap / 2) return CapVerdict::kLiveSetTooLarge;
  if (hit.largest_refused > hit.cap / 2) return CapVerdict::kRequestTooLarge;
  if (hit.hits_in_a_row >= kCapHitsInARowLimit) return CapVerdict::kRepeated;
  return CapVerdict::kGarbage;
}

// CapVerdictReason spells the limit out; keep the two together.
static_assert(kCapHitsInARowLimit == 3, "update CapVerdictReason's kRepeated text");

constexpr const char* CapVerdictReason(CapVerdict verdict) {
  switch (verdict) {
    case CapVerdict::kLiveSetTooLarge: return "over half the cap is still live after collection";
    case CapVerdict::kRequestTooLarge: return "one allocation alone asked for over half the cap";
    case CapVerdict::kRepeated: return "the cap was reached on 3 calls in a row";
    case CapVerdict::kGarbage: return "garbage outran the collector";
  }
  return "";
}

}  // namespace nevr_script

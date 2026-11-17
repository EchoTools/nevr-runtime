#include "cdn/cdn_boot_fetch.h"

#include <cstddef>
#include <limits>

#include <nlohmann/json.hpp>

#include "cdn/cdn_format.h"
#include "core/cargo_session_pin.h"

namespace nevr_cdn {
namespace {

using json = nlohmann::json;

constexpr const char* kCauseSettled = "settled";
constexpr const char* kCauseUndeliverable = "undeliverable";
constexpr const char* kCauseNoSpace = "no_space";
constexpr const char* kKeyHash = "manifest_sha256";
constexpr const char* kKeyCause = "cause";
constexpr const char* kKeyFree = "free_bytes";
constexpr const char* kKeyNeed = "need_bytes";
constexpr std::uint64_t kLargest = std::numeric_limits<std::uint64_t>::max();

// The sum of two byte counts, held at the largest value rather than wrapped.
std::uint64_t SumSaturating(std::uint64_t a, std::uint64_t b) noexcept { return a > kLargest - b ? kLargest : a + b; }

// Whether the label is fetched by this policy: required, or on and not cut by required_only (the plan's MakePlan applies the same rule).
bool PinLabelOn(const std::string& label, const Policy& policy) {
  const LabelState state = StateOf(label, policy);
  if (state == LabelState::kRequired) return true;
  return state == LabelState::kOn && !policy.required_only;
}

// The label has a record for this platform: an entry, or a record of 0 bytes (a record of no platform belongs to the platform the plan is made for).
bool HasRecords(const Manifest& m, const std::string& label, const Platform& platform) {
  for (const Entry& e : m.entries) {
    if (e.label == label && e.platform == platform.name) return true;
  }
  for (const EmptyRecord& z : m.empties) {
    if (z.label == label && (z.platform == platform.name || z.platform.empty())) return true;
  }
  return false;
}

// One entry of the label for this platform is not held by the store.
bool HasMissingObject(const BootFetchInput& in, const std::string& label) {
  for (const Entry& e : in.saved->entries) {
    if (e.label != label || e.platform != in.platform.name) continue;
    if (!in.held || !in.held(e)) return true;
  }
  return false;
}

// The stamp settles the saved manifest: it names that manifest's hash, and its cause is one a retry of the same manifest cannot cure, or
// it is no_space and the free space has not grown by what the label needed.
bool StampSettles(const BootFetchInput& in) {
  const BootFetchStamp* s = in.stamp;
  if (s == nullptr || in.saved_sha256.empty() || s->manifest_sha256 != in.saved_sha256) return false;
  if (s->cause == BootFetchCause::kSettled || s->cause == BootFetchCause::kUndeliverable) return true;
  if (!in.free_space) return false;
  std::uint64_t free_now = 0;
  if (!in.free_space(&free_now)) return false;
  return free_now < SumSaturating(s->free_bytes, s->need_bytes);
}

// The reason one pin label needs a fetch, or kNone when the saved manifest holds it whole.
BootFetchReason ReasonFor(const BootFetchInput& in, const std::string& label) {
  if (in.saved == nullptr) return BootFetchReason::kNoSavedManifest;
  if (in.marker) return BootFetchReason::kRefetchMarker;
  const bool absent = !HasRecords(*in.saved, label, in.platform);
  if (!absent && !HasMissingObject(in, label)) return BootFetchReason::kNone;
  if (StampSettles(in)) return BootFetchReason::kNone;
  return absent ? BootFetchReason::kLabelAbsent : BootFetchReason::kObjectsMissing;
}

}  // namespace

const char* BootFetchCauseName(BootFetchCause cause) noexcept {
  switch (cause) {
    case BootFetchCause::kSettled:
      return kCauseSettled;
    case BootFetchCause::kUndeliverable:
      return kCauseUndeliverable;
    case BootFetchCause::kNoSpace:
      return kCauseNoSpace;
  }
  return kCauseSettled;
}

bool ParseBootFetchCause(const std::string& text, BootFetchCause* out) noexcept {
  if (text == kCauseSettled) {
    *out = BootFetchCause::kSettled;
    return true;
  }
  if (text == kCauseUndeliverable) {
    *out = BootFetchCause::kUndeliverable;
    return true;
  }
  if (text == kCauseNoSpace) {
    *out = BootFetchCause::kNoSpace;
    return true;
  }
  return false;
}

std::string FormatBootFetchStamp(const BootFetchStamp& stamp) {
  const json doc = {{kKeyHash, stamp.manifest_sha256},
                    {kKeyCause, BootFetchCauseName(stamp.cause)},
                    {kKeyFree, stamp.free_bytes},
                    {kKeyNeed, stamp.need_bytes}};
  return doc.dump();
}

bool ParseBootFetchStamp(const std::string& text, BootFetchStamp* out) {
  const json doc = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) return false;
  const auto hash = doc.find(kKeyHash);
  const auto cause = doc.find(kKeyCause);
  const auto free_bytes = doc.find(kKeyFree);
  const auto need_bytes = doc.find(kKeyNeed);
  if (hash == doc.end() || cause == doc.end() || free_bytes == doc.end() || need_bytes == doc.end()) return false;
  if (!hash->is_string() || !IsSha256Hex(hash->get_ref<const std::string&>())) return false;
  if (!cause->is_string()) return false;
  BootFetchCause parsed = BootFetchCause::kSettled;
  if (!ParseBootFetchCause(cause->get_ref<const std::string&>(), &parsed)) return false;
  if (!free_bytes->is_number_unsigned() || !need_bytes->is_number_unsigned()) return false;
  out->manifest_sha256 = hash->get_ref<const std::string&>();
  out->cause = parsed;
  out->free_bytes = free_bytes->get<std::uint64_t>();
  out->need_bytes = need_bytes->get<std::uint64_t>();
  return true;
}

const char* BootFetchReasonName(BootFetchReason reason) noexcept {
  switch (reason) {
    case BootFetchReason::kNone:
      return "none";
    case BootFetchReason::kNoSavedManifest:
      return "no_saved_manifest";
    case BootFetchReason::kRefetchMarker:
      return "refetch_marker";
    case BootFetchReason::kLabelAbsent:
      return "label_absent";
    case BootFetchReason::kObjectsMissing:
      return "objects_missing";
  }
  return "none";
}

BootFetchDecision NeedsBootFetch(const BootFetchInput& input) {
  BootFetchDecision none;
  if (input.boot_only.empty() || input.signed_only_refuses || input.last_update_trial) return none;
  for (std::size_t i = 0; i < nevr_cargo_pin::kCargoPinLabelCount; ++i) {
    const std::string label = nevr_cargo_pin::kCargoPinLabels[i];
    if (!PinLabelOn(label, input.policy)) continue;
    const BootFetchReason reason = ReasonFor(input, label);
    if (reason != BootFetchReason::kNone) {
      BootFetchDecision d;
      d.fetch = true;
      d.reason = reason;
      d.label = label;
      return d;
    }
  }
  return none;
}

}  // namespace nevr_cdn

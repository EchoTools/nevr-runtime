#pragma once

// The one rule for the label a plugin gives a resource it registers (#2215, #2161).
//
// A label is written into log lines (quoted) and into the login's JSON, so it must be able to do neither harm: 1 to
// kMaxLabelBytes bytes of printable ASCII (0x20 to 0x7e) with no double quote and no backslash. Anything else is refused
// where it enters, with kBadArgumentStatus and the reason `bad_label`, and no line carries its bytes (its length and the
// rule are logged instead).
//
// Std types only, so the unit test and the resource-set code link it with no host code. The resource override registry
// (patch/resource_override.cpp) and the resource sets (#2161) both call Ok; neither keeps a second copy of the rule.

#include <cstddef>
#include <cstring>

namespace nevr_resource_label {

inline constexpr std::size_t kMaxLabelBytes = 64;

/// NEVR_RESOURCES_BAD_ARGUMENT, the status #2161's register_resources returns for a bad label. The enum lives in the plugin
/// SDK on #2161's branch; this is its value for the refusal line until that lands.
inline constexpr int kBadArgumentStatus = 1;

inline constexpr const char* kReason = "bad_label";

/// The rule, in words, as a refusal line names it.
inline constexpr const char* kRule = "a label is 1 to 64 bytes of printable ASCII with no double quote or backslash";

/// Whether `label` follows the rule. A null label does not.
inline bool Ok(const char* label) {
  if (label == nullptr) return false;
  const std::size_t n = std::strlen(label);
  if (n == 0 || n > kMaxLabelBytes) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(label[i]);
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
  }
  return true;
}

/// The length a refusal line reports in place of the bytes (0 for a null label).
inline std::size_t Bytes(const char* label) { return label == nullptr ? 0 : std::strlen(label); }

}  // namespace nevr_resource_label

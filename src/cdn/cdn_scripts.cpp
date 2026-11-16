#include "cdn/cdn_scripts.h"

#include <algorithm>

namespace nevr_cdn {

std::vector<std::string> ScriptLabels(const Manifest& manifest, const Platform& platform) {
  std::vector<std::string> labels;
  for (const ScriptEntry& s : manifest.scripts) {
    if (s.platform != platform.name) continue;
    if (std::find(labels.begin(), labels.end(), s.label) == labels.end()) labels.push_back(s.label);
  }
  return labels;
}

bool ListsScript(const Manifest& manifest, const Platform& platform, const std::string& label, std::uint64_t name) {
  for (const ScriptEntry& s : manifest.scripts) {
    if (s.label == label && s.platform == platform.name && s.name == name) return true;
  }
  return false;
}

// KNOWN-WRONG #2242: no manifest verifier exists in the client yet (PLAN-v5 5.6, the 6.19 check), so no manifest is official.
bool ManifestVerified(const Manifest& /*manifest*/) { return false; }

namespace {
constexpr const char* kConsequenceGeneric = "this label is not mounted; its levels run on the game's own data";
constexpr const char* kConsequenceSignedOnly =
    "EchoCombat's new gear is not mounted this run (-require-signed-plugins; the manifest's signature is checked from 6.19)";
constexpr const char* kReasonSignedOnly = "signed_only";
}  // namespace

std::string RefusalConsequence(const std::string& reason) {
  return reason == kReasonSignedOnly ? kConsequenceSignedOnly : kConsequenceGeneric;
}

ScriptRegistration RegisterScriptLabels(const Manifest& manifest, const Platform& platform, const HeldScriptFn& held,
                                        const ClaimScriptFn& claim, const WithdrawLabelFn& withdraw, bool official) {
  ScriptRegistration out;
  for (const std::string& label : ScriptLabels(manifest, platform)) {
    std::vector<std::pair<std::string, std::uint64_t>> taken;  // this label's copies held after this pass, if it is not refused
    bool refused = false;
    for (const ScriptEntry& s : manifest.scripts) {
      if (s.label != label || s.platform != platform.name) continue;
      if (held(label, s.name)) {
        taken.emplace_back(label, s.name);
        continue;
      }
      const ScriptClaim c = claim(label, s, official);
      if (!c.ok) {
        out.refused.push_back(ScriptRefusal{label, s.name, c.reason, c.pack});
        withdraw(label);
        refused = true;
        break;
      }
      taken.emplace_back(label, s.name);
    }
    if (!refused) out.held.insert(out.held.end(), taken.begin(), taken.end());
  }
  return out;
}

}  // namespace nevr_cdn

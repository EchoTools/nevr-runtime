/* The CDN's script registrar, the platform-neutral half (docs/adr/0045-cdn-texture-delivery.md decision 4; PLAN-v5 5.3 item 2, R10b).
 *
 * A label whose manifest `scripts` list names a script is served that script from a copy of the player's own stock file (cdn_manifest.h).
 * The registrar claims each of those copies before the script gate runs (cdn_script_gate.h, cdn_run.cpp GatedPolicy): a claim that is
 * accepted is held and counts as delivered, and a refusal turns its whole label off. The rules here are the platform's only in what the
 * claim and the withdraw do; the PC edge (runtime/content/cdn_scripts_install.cpp) supplies them.
 *
 * Pure: no file system, no Log(). The claim, the held check and the withdraw reach the core through callbacks, so a test drives every rule.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cdn/cdn_manifest.h"
#include "cdn/cdn_script_gate.h"

namespace nevr_cdn {

// The registrar's answer for one claim of a label's script: accepted, or refused with the registrar's reason and, for `pack_hash`, the pack.
struct ScriptClaim {
  bool ok = false;
  std::string reason;
  std::string pack;
};

// `official` is the pass's verifier answer (ManifestVerified): the claim passes it to the provider, which refuses an unofficial copy
// under -require-signed-plugins.
using ClaimScriptFn = std::function<ScriptClaim(const std::string& label, const ScriptEntry& script, bool official)>;
using HeldScriptFn = std::function<bool(const std::string& label, std::uint64_t name)>;
using WithdrawLabelFn = std::function<void(const std::string& label)>;

// The labels of `platform` whose `scripts` list names a script, in manifest order, each once.
std::vector<std::string> ScriptLabels(const Manifest& manifest, const Platform& platform);

// True when the label's own `scripts` list, for `platform`, names the script `name` (the gate's listed rule, 5.2 case 6).
bool ListsScript(const Manifest& manifest, const Platform& platform, const std::string& label, std::uint64_t name);

// The one verifier's answer for `manifest` (PLAN-v7 5.6): true when the manifest is part of the signed chain and its signature checks.
// KNOWN-WRONG #2242: the manifest verifier (the 6.19 signature check) is not in this client, so this answers false, and under
// -require-signed-plugins every copy of a label with scripts is refused signed_only.
bool ManifestVerified(const Manifest& manifest);

// The consequence a refused label's Warning states (PLAN-v7 5.6): the signed-only refusal names its cause and the action.
std::string RefusalConsequence(const std::string& reason);

// The registrar. Labels are taken in ScriptLabels order and, within one label, the scripts of its `scripts` list in manifest order.
// A script `held` already (an earlier pass of this run) is kept without a claim. Any other is claimed with the pass's `official`
// fact. The first refusal ends its label: the label is withdrawn once, every copy of the label is left out of `held` (the withdraw
// drops those the earlier pass held too), the refusal is recorded, and the label's later scripts are not claimed. Other labels are
// unaffected.
ScriptRegistration RegisterScriptLabels(const Manifest& manifest, const Platform& platform, const HeldScriptFn& held,
                                        const ClaimScriptFn& claim, const WithdrawLabelFn& withdraw, bool official);

}  // namespace nevr_cdn

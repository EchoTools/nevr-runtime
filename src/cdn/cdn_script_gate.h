/* The script gate (docs/adr/0045-cdn-texture-delivery.md decision 4): a label whose records of the script type name a script the
 * game cannot load is not mounted. The game has no existence check before a script load: a CScriptResource record names a DLL
 * by hash, the game loads `scripts\<hash16>.dll`, and a record whose file is missing is a crash in the load, not a refusal. So
 * a label is mounted only when every script its records name is delivered.
 *
 * Which records count. Every record of type nevr_resource_sets::kScriptType among a label's entries and records of 0 bytes
 * (`empty`) that name the plan's platform, whatever its size: a 4-byte entry is a script record as much as an empty one. The gate
 * runs before both `Mount` calls of cdn_run.cpp (the boot mount from the saved manifest and the fetch's), because `Mount` reads only
 * a label's state.
 *
 * A script is delivered when it is `present` (a file on disk under that name: a stock script, or one a content pack serves) or
 * `held` (a `scripts` entry of the label that the registrar accepted this run: a copy of the player's own stock file served
 * under the new name). A name in the label's own `scripts` list must be held; a file already on disk under that name is not ours
 * (it may be a third-party file) and the game would load it, so the label is refused as `script_shadowed`.
 *
 * Pure: the file system, the registrar and the manifest's `scripts` list reach the gate through callbacks, so a test drives every rule.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cdn/cdn_env.h"
#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"

namespace nevr_cdn {

// Reasons a label is gated. The registrar's own refusal reasons (stock_hash, pack_hash, duplicate, ...) are named by the registrar.
inline constexpr const char* kGateUndelivered = "script_undelivered";
inline constexpr const char* kGateShadowed = "script_shadowed";

// One label the gate turns off, and the scripts that are the reason.
struct GatedLabel {
  std::string label;
  std::string reason;
  std::vector<std::uint64_t> scripts;  // the scripts that name the reason, in the order the manifest names them
  std::string pack;                    // the content pack's id, when the registrar refused a claim as `pack_hash`
};

// One claim the registrar refused: the label it belongs to, the script, the registrar's reason and, for `pack_hash`, the pack.
struct ScriptRefusal {
  std::string label;
  std::uint64_t script = 0;
  std::string reason;
  std::string pack;
};

// What the registrar did for a manifest: the (label, script) pairs it accepted this run, and the claims it refused. A refusal
// turns its whole label off (the registrar withdraws the label's other claims).
struct ScriptRegistration {
  std::vector<std::pair<std::string, std::uint64_t>> held;
  std::vector<ScriptRefusal> refused;
};

// Runs the registrar for every label of a manifest that lists scripts, before the gate. Unset: nothing is held, nothing is refused.
using ScriptRegistrar = std::function<ScriptRegistration(const Manifest&, const Platform&)>;

// `<hash>` as the game's file name stem: 16 lowercase hex digits, zero-padded.
std::string ScriptHash16(std::uint64_t hash);

// The labels of `platform` to turn off. `present(hash)`: a file `scripts\<hash16>.dll` is on disk. `held(label, hash)`: the registrar
// accepted it for that label this run. `listed(label, hash)`: the label's own `scripts` list names it. A label whose scripts are
// both shadowed and undelivered is reported as shadowed, with the shadowed names.
std::vector<GatedLabel> GateScripts(const Manifest& manifest, const Platform& platform,
                                    const std::function<bool(std::uint64_t)>& present,
                                    const std::function<bool(const std::string&, std::uint64_t)>& held,
                                    const std::function<bool(const std::string&, std::uint64_t)>& listed);

// The labels the registrar refused a claim of, one entry per label, in the order of the refusals.
std::vector<GatedLabel> GateRefusals(const std::vector<ScriptRefusal>& refused);

// A copy of the policy with every gated label off.
Policy ApplyGate(const std::vector<GatedLabel>& gated, const Policy& policy);

// One Warning per gated label whose state in `policy` is not off (a label the player turned off has nothing to refuse):
// `cdn_mount label= result=refused reason= scripts= source= consequence=`; `script_shadowed` adds `path=` (the first file under
// `script_dir`), `pack_hash` adds `pack=`.
void SayGated(Env& env, const std::vector<GatedLabel>& gated, const Policy& policy, const std::string& script_dir, const char* source);

}  // namespace nevr_cdn

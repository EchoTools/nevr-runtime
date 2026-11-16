#include "cdn/cdn_script_gate.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "core/resource_set_registry.h"

namespace nevr_cdn {

namespace {

constexpr unsigned kHash16Digits = 16;

// The script names of a label: its entries of this platform and its records of 0 bytes that name this platform (or none, a top-level
// record), of the script type, in manifest order, each once.
std::vector<std::uint64_t> ScriptsOf(const Manifest& manifest, const Platform& platform, const std::string& label) {
  std::vector<std::uint64_t> names;
  const auto add = [&names](std::uint64_t name) {
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  };
  for (const Entry& e : manifest.entries) {
    if (e.label == label && e.platform == platform.name && e.type == nevr_resource_sets::kScriptType) add(e.name);
  }
  for (const EmptyRecord& e : manifest.empties) {
    if (e.label == label && (e.platform.empty() || e.platform == platform.name) && e.type == nevr_resource_sets::kScriptType) add(e.name);
  }
  return names;
}

std::string JoinHashes(const std::vector<std::uint64_t>& hashes) {
  std::string text;
  for (const std::uint64_t h : hashes) {
    if (!text.empty()) text += ",";
    text += ScriptHash16(h);
  }
  return text;
}

}  // namespace

std::string ScriptHash16(std::uint64_t hash) {
  char text[kHash16Digits + 1];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
  return text;
}

std::vector<GatedLabel> GateScripts(const Manifest& manifest, const Platform& platform,
                                    const std::function<bool(std::uint64_t)>& present,
                                    const std::function<bool(const std::string&, std::uint64_t)>& held,
                                    const std::function<bool(const std::string&, std::uint64_t)>& listed) {
  std::vector<GatedLabel> gated;
  for (const std::string& label : manifest.labels) {
    std::vector<std::uint64_t> undelivered;
    std::vector<std::uint64_t> shadowed;
    for (const std::uint64_t name : ScriptsOf(manifest, platform, label)) {
      if (listed(label, name)) {
        if (held(label, name)) continue;
        (present(name) ? shadowed : undelivered).push_back(name);
      } else if (!present(name) && !held(label, name)) {
        undelivered.push_back(name);
      }
    }
    if (!shadowed.empty()) {
      gated.push_back(GatedLabel{label, kGateShadowed, shadowed, std::string()});
    } else if (!undelivered.empty()) {
      gated.push_back(GatedLabel{label, kGateUndelivered, undelivered, std::string()});
    }
  }
  return gated;
}

std::vector<GatedLabel> GateRefusals(const std::vector<ScriptRefusal>& refused) {
  std::vector<GatedLabel> gated;
  for (const ScriptRefusal& r : refused) {
    const auto it = std::find_if(gated.begin(), gated.end(), [&r](const GatedLabel& g) { return g.label == r.label; });
    if (it == gated.end()) {
      gated.push_back(GatedLabel{r.label, r.reason, {r.script}, r.pack});
    } else {
      it->scripts.push_back(r.script);
    }
  }
  return gated;
}

Policy ApplyGate(const std::vector<GatedLabel>& gated, const Policy& policy) {
  Policy out = policy;
  for (const GatedLabel& g : gated) out.overrides[g.label] = LabelState::kOff;
  return out;
}

void SayGated(Env& env, const std::vector<GatedLabel>& gated, const Policy& policy, const std::string& script_dir, const char* source) {
  for (const GatedLabel& g : gated) {
    if (StateOf(g.label, policy) == LabelState::kOff) continue;
    std::vector<Field> fields = {{"label", g.label},
                                 {"result", "refused"},
                                 {"reason", g.reason},
                                 {"scripts", JoinHashes(g.scripts)},
                                 {"source", source}};
    const char* consequence = "this label is not mounted; its levels run on the game's own data";
    if (g.reason == kGateShadowed) {
      fields.push_back({"path", script_dir + ScriptHash16(g.scripts.front()) + ".dll"});
      consequence = "this label is not mounted: a file already on disk under the name of a script it serves would be loaded in its place; remove the file to get this label";
    } else if (!g.pack.empty()) {
      fields.push_back({"pack", g.pack});
      consequence = "the installed content pack serves this script; remove the pack to get this label";
    }
    fields.push_back({"consequence", consequence});
    env.Event(EventLevel::kWarn, "cdn_mount", fields);
  }
}

}  // namespace nevr_cdn

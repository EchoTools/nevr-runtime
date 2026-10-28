#include "quest/plugin/plugin_plan.h"

#include <utility>

#include <nlohmann/json.hpp>

namespace nevr_quest::plugin {

const char* OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::kLoaded: return "loaded";
    case Outcome::kRefused: return "refused";
    case Outcome::kDisabled: return "disabled";
    case Outcome::kNotStarted: return "not_started";
    case Outcome::kFailed: return "failed";
  }
  return "unknown";
}

const char* PhaseName(PluginPhase phase) { return phase == PluginPhase::Early ? "early" : "normal"; }

std::string DefaultFile(const std::string& name) { return "lib" + name + ".so"; }

namespace {

bool HasSeparator(const std::string& file) { return file.find_first_of("/\\") != std::string::npos; }

// The shared duplicate rule (runtime/ext/plugin_load_plan.h): `it` names a file an earlier, still-enabled claim holds.
// dlopen hands back the module it already mapped for such a repeat, which would run the plugin's init twice.
bool NamesAClaimedFile(const std::vector<PluginLoadItem>& claims, const PluginLoadItem& it) {
  std::vector<PluginLoadItem> probe = claims;
  probe.push_back(it);
  return DuplicatePluginEntry(probe, probe.size() - 1) >= 0;
}

void Settle(PlanEntry& entry, Outcome outcome, const char* token) {
  entry.pending = false;
  entry.outcome = outcome;
  entry.reasonToken = token;
}

}  // namespace

std::vector<PlanEntry> BuildPlan(std::vector<PluginLoadItem> items, bool earlySupported) {
  std::vector<PlanEntry> plan;
  plan.reserve(items.size());
  // The items as the duplicate rule sees them: an entry that will not load does not claim its file, so a later entry
  // naming the same file is not refused for it.
  std::vector<PluginLoadItem> claims;
  claims.reserve(items.size());
  for (PluginLoadItem& item : items) {
    PlanEntry entry;
    // The shared parser's default `file` is `<name>.dll`; on Quest the default is `lib<name>.so`.
    if (item.file == item.name + ".dll") item.file = DefaultFile(item.name);
    entry.phase = item.early ? PluginPhase::Early : PluginPhase::Normal;
    entry.item = std::move(item);
    const PluginLoadItem& it = entry.item;
    if (!it.enabled) {
      Settle(entry, Outcome::kDisabled, reason::kDisabled);
    } else if (HasSeparator(it.file)) {
      Settle(entry, Outcome::kRefused, reason::kFileOutsideDirectory);
    } else if (NamesAClaimedFile(claims, it)) {
      Settle(entry, Outcome::kRefused, reason::kDuplicateFile);
    } else if (it.early && !earlySupported) {
      Settle(entry, Outcome::kRefused, reason::kEarlyUnsupported);
    }
    PluginLoadItem claim = entry.item;
    claim.enabled = entry.pending;
    claims.push_back(std::move(claim));
    plan.push_back(std::move(entry));
  }
  return plan;
}

std::string ArgKeys(const std::string& argsJson) {
  const nlohmann::json parsed = nlohmann::json::parse(argsJson, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_object()) return "";
  std::string keys;
  for (auto it = parsed.begin(); it != parsed.end(); ++it) {
    if (!keys.empty()) keys += ',';
    keys += it.key();
  }
  return keys;
}

}  // namespace nevr_quest::plugin

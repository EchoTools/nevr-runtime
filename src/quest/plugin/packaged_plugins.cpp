#include "quest/plugin/packaged_plugins.h"

#include <cctype>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "quest/plugin/plugin_plan.h"

namespace nevr_quest::plugin {

namespace {

bool Refuse(std::string* reason, const char* token) {
  *reason = token;
  return false;
}

std::string Lower(const std::string& text) {
  std::string out = text;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

// The rule the loader applies to a plan entry's file (plugin_plan.cpp FileLeavesDirectory): one entry of the directory.
bool FileIsOneEntry(const std::string& file) {
  if (file.empty() || file == "." || file == "..") return false;
  return file.find_first_of(std::string("/\\") + '\0') == std::string::npos;
}

bool IsSha256Hex(const std::string& text) {
  if (text.size() != 64) return false;
  for (const char c : text) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    if (!digit && !lower) return false;
  }
  return true;
}

// The string value of `key` in `entry`, or nullptr when it is absent or not a string.
const std::string* StringField(const nlohmann::json& entry, const char* key) {
  const auto it = entry.find(key);
  return it != entry.end() && it->is_string() ? &it->get_ref<const std::string&>() : nullptr;
}

bool BoolField(const nlohmann::json& entry, const char* key, bool* out) {
  const auto it = entry.find(key);
  if (it == entry.end() || !it->is_boolean()) return false;
  *out = it->get<bool>();
  return true;
}

}  // namespace

bool ParseManifest(const std::string& text, std::vector<PackagedPlugin>* out, std::string* reason) {
  out->clear();
  reason->clear();
  if (text.size() > kMaxManifestBytes) return Refuse(reason, "too_large");
  const nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) return Refuse(reason, "not_json");
  const auto schema = doc.find("schema");
  if (schema == doc.end() || !schema->is_number_integer() || schema->get<long long>() != 1) {
    return Refuse(reason, "bad_schema");
  }
  const auto plugins = doc.find("plugins");
  if (plugins == doc.end() || !plugins->is_array()) return Refuse(reason, "bad_entry");
  if (plugins->size() > kMaxPackagedPlugins) return Refuse(reason, "too_many");

  std::vector<PackagedPlugin> parsed;
  std::set<std::string> files;  // lower-cased: two entries may not name one file in any case
  for (const nlohmann::json& entry : *plugins) {
    if (!entry.is_object()) return Refuse(reason, "bad_entry");
    const std::string* name = StringField(entry, "name");
    const std::string* file = StringField(entry, "file");
    const std::string* sha = StringField(entry, "sha256");
    PackagedPlugin packaged;
    if (name == nullptr || name->empty() || file == nullptr || sha == nullptr) return Refuse(reason, "bad_entry");
    if (!BoolField(entry, "enabled", &packaged.enabled) || !BoolField(entry, "early", &packaged.early)) {
      return Refuse(reason, "bad_entry");
    }
    if (*file != DefaultFile(*name) || !FileIsOneEntry(*file)) return Refuse(reason, "bad_file");
    if (!IsSha256Hex(*sha)) return Refuse(reason, "bad_sha256");
    if (!files.insert(Lower(*file)).second) return Refuse(reason, "duplicate_file");
    packaged.name = *name;
    packaged.file = *file;
    packaged.sha256 = *sha;
    parsed.push_back(std::move(packaged));
  }
  *out = std::move(parsed);
  return true;
}

const char* SourceName(Source source) { return source == Source::kPackaged ? "packaged" : "config"; }

std::vector<PlannedItem> MergePackaged(const std::vector<PackagedPlugin>& packaged, std::vector<PluginLoadItem> config) {
  std::vector<PlannedItem> merged;
  merged.reserve(packaged.size() + config.size());
  for (const PackagedPlugin& p : packaged) {
    PlannedItem planned;
    planned.item.name = p.name;
    planned.item.file = p.file;
    planned.item.enabled = p.enabled;
    planned.item.early = p.early;
    planned.item.args_json = "{}";
    planned.source = Source::kPackaged;
    planned.expectedSha256 = p.sha256;
    merged.push_back(std::move(planned));
  }
  std::vector<bool> overridden(merged.size(), false);
  std::vector<PlannedItem> configOnly;
  for (PluginLoadItem& entry : config) {
    // The shared parser's default `file` is `<name>.dll`; on Quest it is `lib<name>.so` (plugin_plan.cpp, BuildPlan).
    if (entry.file == entry.name + ".dll") entry.file = DefaultFile(entry.name);
    bool matched = false;
    for (std::size_t i = 0; i < merged.size() && !matched; ++i) {
      if (merged[i].source != Source::kPackaged || overridden[i]) continue;
      if (!EqualsIgnoreCase(merged[i].item.file, entry.file)) continue;
      PlannedItem& target = merged[i];
      target.item.name = entry.name;
      target.item.enabled = entry.enabled;
      target.item.required = entry.required;
      target.item.target = entry.target;
      target.item.args_json = entry.args_json;
      target.item.args_replaced_keys = entry.args_replaced_keys;
      target.item.early = target.item.early || entry.early;  // config can set early, never clear the manifest's
      target.overridden = true;
      overridden[i] = true;
      matched = true;
    }
    if (matched) continue;
    PlannedItem planned;
    planned.item = std::move(entry);
    planned.source = Source::kConfig;
    configOnly.push_back(std::move(planned));
  }
  for (PlannedItem& planned : configOnly) merged.push_back(std::move(planned));
  return merged;
}

}  // namespace nevr_quest::plugin

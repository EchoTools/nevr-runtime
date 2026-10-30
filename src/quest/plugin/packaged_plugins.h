// The plugins packaged in the APK (docs/adr/0019-quest-plugin-loader.md, section 2): the manifest the repack writes beside
// them, and the merge of that manifest with the `plugins:` list of config.yaml. Pure: std types and nlohmann only, no Android
// and no game headers, so the host test and the NDK build compile this one source.
//
// A packaged plugin loads without any config.yaml entry. config.yaml overrides it: an entry with the same file (ASCII
// case-insensitive) changes its enabled, required, args, target and early, in place; an entry naming another file is a
// config-only entry, as before.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "runtime/ext/plugin_load_plan.h"

namespace nevr_quest::plugin {

// The file the repack writes into lib/arm64-v8a. Its name is lib*.so so that the installer extracts it beside the plugins.
inline constexpr const char* kManifestFile = "libnevr_plugin_manifest.so";
inline constexpr std::size_t kMaxManifestBytes = 64 * 1024;
inline constexpr std::size_t kMaxPackagedPlugins = 64;

// One packaged entry: the file the repack staged, the hash it staged, and the flags the repack recorded.
struct PackagedPlugin {
  std::string name;
  std::string file;
  std::string sha256;  // 64 lowercase hex characters, the sha-256 of the staged file
  bool enabled = true;
  bool early = false;
};

// Parses the manifest text. true: `out` holds the entries (possibly none) and `reason` is empty. false: `reason` is a manifest
// reason token (not_json, bad_schema, bad_entry, bad_file, bad_sha256, duplicate_file, too_large, too_many) and `out` is empty.
// One bad entry refuses the whole manifest; a partial manifest is never used.
bool ParseManifest(const std::string& text, std::vector<PackagedPlugin>* out, std::string* reason);

// Where a planned entry came from.
enum class Source { kConfig, kPackaged };
const char* SourceName(Source source);  // "config" or "packaged"

// One entry of the merged plan. `expectedSha256` is the packaged file's hash (empty for a config-only entry); the loader
// refuses the file when its bytes do not hash to it.
struct PlannedItem {
  PluginLoadItem item;
  Source source = Source::kConfig;
  bool overridden = false;  // a config entry named this packaged file
  std::string expectedSha256;
};

// The merge (D3): the packaged entries in manifest order, each overridden in place by the first config entry that names its
// file, then the config-only entries in config order. A config file is normalised first (`<name>.dll` becomes `lib<name>.so`).
std::vector<PlannedItem> MergePackaged(const std::vector<PackagedPlugin>& packaged, std::vector<PluginLoadItem> config);

}  // namespace nevr_quest::plugin

#include "quest/data/data_step.h"

#include <dirent.h>

#include <algorithm>
#include <cstdio>

#include "quest/data/sha256.h"
#include "quest/data/staging.h"

namespace nevr_quest::data {

namespace {

std::string Mib(std::uint64_t bytes) { return std::to_string((bytes + (1024u * 1024u) - 1) / (1024u * 1024u)); }

const char* ActionName(DataAction action) noexcept {
  switch (action) {
    case DataAction::kUse: return "use";
    case DataAction::kFetch: return "fetch";
    case DataAction::kReplace: return "replace";
  }
  return "unknown";
}

// The names in `dir`, or none when it cannot be opened.
std::vector<std::string> ListDirReal(const std::string& dir) {
  std::vector<std::string> names;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return names;
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name != "." && name != "..") names.push_back(name);
  }
  closedir(d);
  return names;
}

constexpr std::size_t kMaxManifestBytes = 16u * 1024u * 1024u;

bool ReadFileReal(const std::string& path, std::string& bytes) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  bytes.clear();
  char buffer[8192];
  bool ok = true;
  for (;;) {
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer), f);
    if (n == 0) {
      ok = std::feof(f) != 0;
      break;
    }
    bytes.append(buffer, n);
    if (bytes.size() > kMaxManifestBytes) {
      ok = false;
      break;
    }
  }
  std::fclose(f);
  return ok;
}

// A folder name goes into the log only when it is plain: the data under a root is not ours.
bool IsPlainName(const std::string& name) noexcept {
  if (name.empty() || name.size() > 32) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-';
  });
}

struct DataVersion {
  std::string folder;           // the folder under `_data`; with no match, the folders that are there; "none" for none
  std::string manifest_sha256;  // the sha256 of the manifest file the game reads; "none" when no folder holds it
};

// Which data a root holds: the folder under `<root>/_data` that holds the manifest the game reads (the last segment of
// kManifestPath) and the sha256 of that file. The folder is read from the directory, never taken from the pin, so a
// root holding another build's data names that build's folder.
DataVersion FindDataVersion(const StepConfig& config, const std::string& root) {
  const std::string manifest_id = std::string(kManifestPath).substr(std::string(kManifestPath).rfind('/') + 1);
  std::vector<std::string> names = config.list_dir ? config.list_dir(root + "/_data") : ListDirReal(root + "/_data");
  std::sort(names.begin(), names.end());
  names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return !IsPlainName(n); }),
              names.end());
  for (const std::string& name : names) {
    const std::string path = root + "/_data/" + name + "/rad15/android/manifests/" + manifest_id;
    if (!config.exists(path.c_str())) continue;
    std::string bytes;
    const bool read = config.read_file ? config.read_file(path, bytes) : ReadFileReal(path, bytes);
    return {name, read ? Sha256Hex(bytes.data(), bytes.size()) : std::string("unreadable")};
  }
  if (names.empty()) return {"none", "none"};
  std::string folders;
  for (std::size_t i = 0; i < names.size() && i < 4; ++i) folders += (i == 0 ? "" : ",") + names[i];
  return {folders, "none"};
}

}  // namespace

const char* StepOutcomeName(StepOutcome outcome) noexcept {
  switch (outcome) {
    case StepOutcome::kUsed: return "used";
    case StepOutcome::kFetched: return "fetched";
    case StepOutcome::kFailed: return "failed";
    case StepOutcome::kMissing: return "missing";
  }
  return "unknown";
}

std::string BuildFloorMessage(const FloorInfo& info) {
  const std::string target = info.root + "/" + data_root::kMarker;
  std::string text = "Echo VR data is missing: " + target + " (game build " + info.build_id + "). ";
  switch (info.status) {
    case FetchStatus::kNoSpace:
      text += "Not enough free space to download it: " + Mib(info.available_bytes) + " MiB free, " +
              Mib(info.needed_bytes) + " MiB needed. Free some space on the headset and start the game again.";
      break;
    case FetchStatus::kBadRoot:
      text += "The folder cannot be created or written. Check the headset storage and start the game again.";
      break;
    case FetchStatus::kRetriesExhausted:
      text += "The download failed (" + info.detail +
              "). Check the network connection and start the game again, or copy the game data to " + target +
              " from a PC.";
      break;
    case FetchStatus::kExtractFailed:
      text += "The downloaded data did not unpack (" + info.detail + "). Start the game again to retry.";
      break;
    case FetchStatus::kSwapFailed:
      text += "The unpacked data could not be put in place (" + info.detail + "). Start the game again to retry.";
      break;
    case FetchStatus::kCancelled:
      text += "The download was cancelled. Start the game again to retry.";
      break;
    case FetchStatus::kOk:
      text += "No failure was reported.";
      break;
  }
  if (info.replacing) text += " The data now in that folder is for another game build and was left as it is.";
  return text;
}

namespace {

// An interrupted install leaves the old tree aside, a staging directory and a part. The decision below reads only
// whether `_data` is there, so these are put right first, on every root the decision could pick (staging.h: run
// RecoverReplaced before CleanLeftovers and before the data decision).
void PutLeftoversRight(const StepConfig& config) {
  for (const std::string& root : RecoveryRootsFor(config)) {
    std::string restored_from;
    const bool recovered = RecoverReplaced(root, &restored_from);
    int failed = 0;
    const int cleaned = CleanLeftovers(root, &failed);
    if (config.on_step && (recovered || cleaned != 0 || failed != 0)) {
      config.on_step({"data_recover",
                      {{"root", root},
                       {"recovered", recovered ? restored_from : std::string()},
                       {"cleaned", std::to_string(cleaned)},
                       {"failed", std::to_string(failed)}}});
    }
  }
}

// The media root alone: where Resolve lands when the legacy root may not be read.
data_root::Resolution ResolveMediaOnly(data_root::ExistsFn exists) noexcept {
  if (data_root::HoldsData(data_root::kMediaRoot, exists)) return {data_root::kMediaRoot, "android_media", true};
  if (exists(data_root::kMediaRoot)) return {data_root::kMediaRoot, "android_media_unmarked", true};
  return {data_root::kMediaRoot, "none", false};
}

// A download left beside data that is in use (whole or not) is ours to remove, when it carries our marker: nothing
// will resume it.
void RemoveUsedPart(const StepConfig& config, const std::string& root) {
  const std::string part = root + "/" + kPartName;
  if (!IsOwned(part)) return;
  const bool removed = RemoveTree(part) && RemoveTree(OwnerMarkerPath(part));
  if (config.on_step) config.on_step({"data_cleanup", {{"part", part}, {"removed", removed ? "true" : "false"}}});
}

}  // namespace

std::vector<std::string> RecoveryRootsFor(const StepConfig& config) {
  std::vector<std::string> roots = config.recovery_roots;
  if (roots.empty()) {
    for (const data_root::Candidate& c : data_root::kCandidates) roots.push_back(c.root);
  }
  if (!config.legacy_root_readable) {
    std::vector<std::string> allowed;
    for (const std::string& root : roots) {
      if (root != data_root::kLegacyRoot) allowed.push_back(root);
    }
    roots.swap(allowed);
  }
  return roots;
}

StepResult RunDataStep(const StepConfig& config, Transport& transport) {
  StepResult result;
  const bool engine_root = !config.engine_root.empty();
  if (!engine_root && !config.legacy_root_readable && config.on_step) {
    config.on_step({"data_legacy_skipped", {{"root", data_root::kLegacyRoot}, {"reason", "no_storage_permission"}}});
  }
  if (config.allow_fetch) PutLeftoversRight(config);
  data_root::Resolution resolution;
  if (engine_root) {
    resolution = data_root::HoldsData(config.engine_root.c_str(), config.exists)
                     ? data_root::Resolution{config.engine_root.c_str(), "engine", true}
                     : data_root::Resolution{config.engine_root.c_str(), "none", false};
  } else {
    resolution = config.legacy_root_readable ? data_root::Resolve(config.exists) : ResolveMediaOnly(config.exists);
  }
  Decision decision = Decide(resolution, config.exists);
  if (engine_root) decision.root = config.engine_root.c_str();
  result.action = decision.action;
  result.root = decision.root;
  if (config.on_step) {
    const DataVersion held = FindDataVersion(config, decision.root);
    config.on_step({"data_check",
                    {{"action", ActionName(decision.action)},
                     {"source", decision.source},
                     {"root", decision.root},
                     {"data_version", held.folder},
                     {"manifest_sha256", held.manifest_sha256}}});
  }
  if (decision.action == DataAction::kUse) {
    if (!config.allow_fetch) {
      result.outcome = StepOutcome::kUsed;
      return result;
    }
    RemoveUsedPart(config, config.fetch_root_override.empty() ? std::string(decision.root) : config.fetch_root_override);
    result.outcome = StepOutcome::kUsed;
    return result;
  }

  if (!config.allow_fetch) {
    result.outcome = StepOutcome::kMissing;
    result.message = kGameDataMissing;
    if (config.present) config.present(result.message);
    if (config.hold) config.hold();
    return result;
  }

  FetchConfig fetch = config.fetch;
  fetch.before_download = config.announce;
  fetch.root = config.fetch_root_override.empty() ? std::string(decision.root) : config.fetch_root_override;
  fetch.replacing = decision.action == DataAction::kReplace;
  const FetchResult fr = RunFetch(fetch, transport);
  result.status = fr.status;
  if (fr.status == FetchStatus::kOk) {
    result.outcome = StepOutcome::kFetched;
    if (config.on_step) {
      const DataVersion fetched = FindDataVersion(config, fetch.root);
      config.on_step({"data_version",
                      {{"root", fetch.root},
                       {"data_version", fetched.folder},
                       {"manifest_sha256", fetched.manifest_sha256}}});
    }
    return result;
  }

  FloorInfo info;
  info.status = fr.status;
  info.detail = fr.detail;
  info.root = fetch.root;
  info.build_id = config.build_id;
  info.replacing = fetch.replacing;
  info.needed_bytes = fr.needed_bytes;
  info.available_bytes = fr.available_bytes;
  result.outcome = StepOutcome::kFailed;
  result.message = BuildFloorMessage(info);
  if (fr.status == FetchStatus::kBadRoot && !config.bad_root_message.empty()) result.message = config.bad_root_message;
  if (config.present) config.present(result.message);
  if (config.hold) config.hold();
  return result;
}

}  // namespace nevr_quest::data

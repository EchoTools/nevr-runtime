#include "quest/data/data_step.h"

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

}  // namespace

const char* StepOutcomeName(StepOutcome outcome) noexcept {
  switch (outcome) {
    case StepOutcome::kUsed: return "used";
    case StepOutcome::kFetched: return "fetched";
    case StepOutcome::kFailed: return "failed";
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
  if (!config.legacy_root_readable && config.on_step) {
    config.on_step({"data_legacy_skipped", {{"root", data_root::kLegacyRoot}, {"reason", "no_storage_permission"}}});
  }
  PutLeftoversRight(config);
  const data_root::Resolution resolution =
      config.legacy_root_readable ? data_root::Resolve(config.exists) : ResolveMediaOnly(config.exists);
  const Decision decision = Decide(resolution, config.exists);
  result.action = decision.action;
  result.root = decision.root;
  if (config.on_step) {
    config.on_step({"data_check",
                    {{"action", ActionName(decision.action)}, {"source", decision.source}, {"root", decision.root}}});
  }
  if (decision.action == DataAction::kUse) {
    RemoveUsedPart(config, config.fetch_root_override.empty() ? std::string(decision.root) : config.fetch_root_override);
    result.outcome = StepOutcome::kUsed;
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

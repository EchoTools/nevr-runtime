#include "quest/data/data_step.h"

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

StepResult RunDataStep(const StepConfig& config, Transport& transport) {
  StepResult result;
  const data_root::Resolution resolution = data_root::Resolve(config.exists);
  const Decision decision = Decide(resolution, config.exists);
  result.action = decision.action;
  result.root = decision.root;
  if (config.on_step) {
    config.on_step({"data_check",
                    {{"action", ActionName(decision.action)}, {"source", decision.source}, {"root", decision.root}}});
  }
  if (decision.action == DataAction::kUse) {
    result.outcome = StepOutcome::kUsed;
    return result;
  }

  FetchConfig fetch = config.fetch;
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
  if (config.present) config.present(result.message);
  if (config.hold) config.hold();
  return result;
}

}  // namespace nevr_quest::data

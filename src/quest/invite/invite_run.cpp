#include "quest/invite/invite_run.h"

#include <chrono>
#include <exception>
#include <functional>
#include <string>
#include <utility>

#include "quest/invite/invite_transport_quest.h"
#include "runtime/patch/invite_service.h"

namespace nevr_quest_invite {

// STUB (slice S1 replaces it with Feature::kInviteCodes). Off in every build until then.
nevr_invite::Switch InviteCodesSwitch() { return nevr_invite::Switch::Off; }

namespace {

// A code's fingerprint is comparable within one run only. The steady clock is not a wall time and is never logged.
std::string RunSalt() { return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()); }

}  // namespace

void Start(RunInputs inputs) {
  const std::function<void(nevr_invite::LogLevel, const std::string&)> log = inputs.log;
  try {
    nevr_invite_service::Settings settings;
    settings.invite_codes = InviteCodesSwitch();
    settings.http_uri = std::move(inputs.http_uri);
    settings.token = std::move(inputs.token);
    settings.log = std::move(inputs.log);
    settings.salt = RunSalt();
    settings.transport_factory = [](const std::string& base, const nevr_invite::TransportOptions& options) {
      return MakeQuestTransport(base, options);
    };
    nevr_invite_service::Start(std::move(settings));
  } catch (const std::exception&) {
    if (log) log(nevr_invite::LogLevel::Error, "[NEVR.INVITE] service start failed reason=exception");
  }
}

void Shutdown() { nevr_invite_service::Shutdown(); }

}  // namespace nevr_quest_invite

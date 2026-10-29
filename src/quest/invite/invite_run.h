#pragma once
// #966 slice S3 (docs/adr/0017): the Quest's start and stop of the invite service. The sentinel starts it with the token auth
// (production_steps.cpp, StartTokenAuth) and stops it on the game's process end and at ShutdownIntegration. The service is the
// shared runtime code (src/runtime/patch/invite_service.h); this file only chooses its switch, its address, its token and its
// transport.
#include <functional>
#include <string>

#include "runtime/patch/invite_config.h"
#include "runtime/patch/invite_worker.h"

namespace nevr_quest_invite {

/// The network.invite_codes switch of this build. A STUB until slice S1 adds the feature to src/quest/sentinel/quest_config.h: it
/// returns Off, so this build starts the service, logs that it is off and sends nothing. Remove it when S1 lands.
nevr_invite::Switch InviteCodesSwitch();

struct RunInputs {
  std::string http_uri;                    // auth.http_uri: the game service's HTTP address
  std::function<std::string()> token;      // the Quest token, read per request; "" when there is none yet
  std::function<void(nevr_invite::LogLevel, const std::string&)> log;
};

/// Starts the invite service for this run with the Quest transport. Its outcome is logged (`service on` or `feature off`); a
/// failure to start is logged, not thrown.
void Start(RunInputs inputs);

/// Aborts a request in flight, drops the queued ones and joins the worker. Safe to call when Start never ran.
void Shutdown();

}  // namespace nevr_quest_invite

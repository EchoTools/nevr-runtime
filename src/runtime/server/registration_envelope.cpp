#include "runtime/server/registration_envelope.h"

namespace nevr_game_server {

std::string FormatRegistrationVersion(std::string_view gitDescribe, std::string_view gitCommit,
                                      std::string_view buildType) {
  std::string version(gitDescribe);
  version += " (";
  version += gitCommit;
  version += " ";
  version += buildType;
  version += ")";
  return version;
}

gameservice::v1::Envelope BuildRegistrationEnvelope(const RegistrationParams& params) {
  gameservice::v1::Envelope envelope;
  auto* registration = envelope.mutable_game_server_registration();
  registration->set_login_session_id(params.loginSessionId);
  registration->set_server_id(params.serverId);
  registration->set_internal_ip_address(params.externalIp);
  registration->set_port(params.port);
  registration->set_region(params.regionId);
  registration->set_version_lock(params.versionLock);
  registration->set_time_step_usecs(params.timeStepUsecs);
  registration->set_version(params.version);
  return envelope;
}

}  // namespace nevr_game_server

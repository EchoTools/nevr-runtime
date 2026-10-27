#pragma once

// The GameServerRegistration envelope the runtime sends to ServerDB. Built in one place: the
// initial registration (GameServerLib::RequestRegistration) and the re-registration after a
// WebSocket reconnect (SetConnectionHandler) both call BuildRegistrationEnvelope, so the two
// cannot drift apart.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gameservice/v1/gameservice.pb.h"

namespace nevr_game_server {

struct RegistrationParams {
  std::string loginSessionId;  // UUID string of the server's login session
  uint64_t serverId = 0;
  std::string externalIp;  // public-facing IP; the protobuf field is named internal_ip_address
  uint32_t port = 0;
  uint64_t regionId = 0;
  uint64_t versionLock = 0;
  uint32_t timeStepUsecs = 0;
  std::string version;
  std::vector<uint64_t> installedLevels;  // level-root symbols (installed_levels, field 9)
  std::vector<std::string> regions;       // regions this server serves (field 10)
  std::vector<std::string> guilds;        // guild ids this server hosts for (field 11)
};

/// "<git describe> (<commit> <build type>)": the version field's content.
std::string FormatRegistrationVersion(std::string_view gitDescribe, std::string_view gitCommit,
                                      std::string_view buildType);

gameservice::v1::Envelope BuildRegistrationEnvelope(const RegistrationParams& params);

}  // namespace nevr_game_server

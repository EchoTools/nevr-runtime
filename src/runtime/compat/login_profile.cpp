#include "runtime/compat/login_profile.h"

namespace LoginProfile {

std::string BuildLoginProfileJson(const LoginProfileInputs& inputs) {
  nlohmann::json profile;
  profile["accountid"] = inputs.account_id;
  profile["displayname"] = inputs.display_name.empty()
                                ? std::to_string(inputs.account_id)
                                : inputs.display_name;
  profile["bypassauth"] = false;
  profile["access_token"] = inputs.access_token;
  profile["password"] = inputs.password;
  profile["nonce"] = "";
  profile["buildversion"] = 631547;
  profile["lobbyversion"] = 0;
  profile["appid"] = 0;
  profile["publisher_lock"] = "";
  profile["hmdserialnumber"] = inputs.hmd_serial_number;
  profile["desiredclientprofileversion"] = 0;

  auto& identity = profile["nevr_identity"];
  identity["version"] = inputs.project_version;
  identity["commit"] = inputs.git_commit;
  identity["build"] = inputs.git_describe;
  identity["build_type"] = inputs.build_type;
  profile["nevr_social"] = inputs.social_level;
  profile["nevr_plugins"] = inputs.plugins;

  auto& system = profile["system_info"];
  system["headset_type"] = inputs.headset_type;
  system["driver_version"] = inputs.driver_version;
  system["network_type"] = inputs.network_type;
  system["video_card"] = inputs.video_card;
  system["cpu"] = inputs.cpu;
  system["num_physical_cores"] = inputs.physical_cores;
  system["num_logical_cores"] = inputs.logical_cores;
  system["memory_total"] = inputs.memory_total_mb;
  system["memory_used"] = inputs.memory_used_mb;
  system["dedicated_gpu_memory"] = 0;

  return profile.dump();
}

}  // namespace LoginProfile

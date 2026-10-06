#include "runtime/compat/login_profile.h"

namespace LoginProfile {

// The profile matches the game's SNSLogInRequestv2 format. It is built with
// nlohmann::json, never by formatting a string: a hand-built format string cannot
// escape its own values, so a version string or display name containing a double
// quote would produce malformed JSON the server rejects.
std::string BuildLoginProfileJson(const LoginProfileInputs& inputs) {
  nlohmann::json profile;
  profile["accountid"] = inputs.account_id;
  // An empty name means the account's name is not known. The account id is a
  // true, unique identifier; a shared placeholder would make every client
  // announce the same name.
  profile["displayname"] = inputs.display_name.empty()
                                ? std::to_string(inputs.account_id)
                                : inputs.display_name;
  profile["bypassauth"] = false;
  profile["access_token"] = inputs.access_token;
  // The account requires password authentication: without this field the server
  // rejects the login with "LOGIN FAILURE: status=400 ... account requires
  // password authentication".
  profile["password"] = inputs.password;
  profile["nonce"] = "";
  profile["buildversion"] = 631547;
  profile["lobbyversion"] = 0;
  profile["appid"] = 0;
  profile["publisher_lock"] = "";
  // The caller supplies the serial the stock client would send, so a shared
  // constant does not make every player a strong alt of every other. The value
  // is an identifier and is never logged; callers log only its source and length.
  profile["hmdserialnumber"] = inputs.hmd_serial_number;
  profile["desiredclientprofileversion"] = 0;

  auto& identity = profile["nevr_identity"];
  identity["version"] = inputs.project_version;
  identity["commit"] = inputs.git_commit;
  identity["build"] = inputs.git_describe;
  identity["build_type"] = inputs.build_type;
  // The social message level this runtime understands. The server sends a newer
  // social message only to a session that declared its level.
  profile["nevr_social"] = inputs.social_level;
  // Every configured plugin with what the loader did with it: loaded (ver/api/
  // caps), failed (error), or disabled. A top-level key beside nevr_identity and
  // nevr_social, carried as a JSON array rather than a string-escaped copy of one.
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

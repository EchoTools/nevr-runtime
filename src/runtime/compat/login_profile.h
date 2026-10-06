#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace LoginProfile {

struct LoginProfileInputs {
  uint64_t account_id = 0;
  std::string display_name;
  std::string access_token;
  std::string password;
  std::string hmd_serial_number = "unknown";
  std::string headset_type = "No VR";
  std::string driver_version;
  std::string network_type;
  std::string video_card;
  std::string cpu;
  uint64_t physical_cores = 0;
  uint64_t logical_cores = 0;
  uint64_t memory_total_mb = 0;
  uint64_t memory_used_mb = 0;
  std::string project_version;
  std::string git_commit;
  std::string git_describe;
  std::string build_type;
  uint64_t social_level = 0;
  nlohmann::json plugins = nlohmann::json::array();
};

std::string BuildLoginProfileJson(const LoginProfileInputs& inputs);

}  // namespace LoginProfile

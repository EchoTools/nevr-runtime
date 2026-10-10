#include "quest/diag/hwdump_field.h"

#include <cstring>

namespace nevr_quest::hwdump {

nlohmann::json Ok(const std::string& source, nlohmann::json value) {
  nlohmann::json f = nlohmann::json::object();
  f["ok"] = true;
  f["source"] = source;
  f["value"] = std::move(value);
  return f;
}

nlohmann::json Fail(const std::string& source, const std::string& error) {
  nlohmann::json f = nlohmann::json::object();
  f["ok"] = false;
  f["source"] = source;
  f["error"] = error.empty() ? std::string("unknown error") : error;
  return f;
}

std::string ErrnoText(const char* call, int err) {
  return std::string(call) + " failed: " + std::strerror(err) + " (errno " + std::to_string(err) + ")";
}

void CountFields(const nlohmann::json& node, std::size_t* total, std::size_t* failed) {
  if (node.is_object()) {
    const auto ok = node.find("ok");
    if (ok != node.end() && ok->is_boolean() && node.contains("source")) {
      ++*total;
      if (!ok->get<bool>()) ++*failed;
      return;  // a field's value is data, not more fields
    }
    for (const auto& item : node.items()) CountFields(item.value(), total, failed);
  } else if (node.is_array()) {
    for (const auto& item : node) CountFields(item, total, failed);
  }
}

}  // namespace nevr_quest::hwdump
